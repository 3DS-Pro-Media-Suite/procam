#include "burst_shot.h"
#include <3ds.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "auto_exposure.h"
#include "background_merge.h"
#include "cam_settings.h"
#include "cam_stream.h"
#include "dual_burst.h"
#include "dual_stream.h"
#include "fast_view.h"
#include "hot_pixel_learner.h"
#include "math_util.h"
#include "merge.h"
#include "photo_writer.h"
#include "raw_view.h"
#include "review.h"
#include "saver.h"
#include "sensor_exposure.h"
#include "settings.h"
#include "shot_mode.h"
#include "shot_names.h"
#include "still_capture.h"
#include "time_units.h"
#include "toast.h"

#define HOT_PIXEL_BYTES (2 * sizeof(u16)) // one defect map entry: x, y
// Memory a MERGE / HDR shot leaves free besides its frames: the saver's JPG render of a merge and the merge's
// own buffers.
#define MERGE_HEADROOM (9u * 1024 * 1024)

#define PAIR_FRAMES 1 // a 3D shot takes one frame from each camera

#define BURST_EXTRA_FRAMES 6 // frames a burst may wait for: a settled exposure, a port not armed yet
#define BURST_TIMEOUT_SLACK_MS 1500

#define PARTIAL_BURST_MIN_FRAMES 2 // a short burst is merged from this many frames of camera A
// The largest code the merge's noise model takes. A gain read back from the sensor may be odd, one above
// SENSOR_GAIN_CODE_MAX.
#define MERGE_GAIN_CODE_MAX 127

// What one MERGE or HDR shot takes.
typedef struct {
	const Settings *settings;
	int frameCount; // frames per camera
	bool outer; // an outer camera is in use, so camera B can be captured too
	bool dual; // the dual stream runs
	bool wantB; // camera B's frames come from the dual stream
} BurstPlan;

typedef enum {
	BURST_CAPTURED,
	BURST_FAILED,
	BURST_NO_MEMORY,
} BurstOutcome;

// An owned copy of a camera's defect map, NULL when it is empty. The merge runs seconds after its shot, while
// the learner rebuilds its map every few seconds.
static u16 *copy_hot_pixel_map(int camera, int *pixelCount)
{
	const u16 *learnedMap;
	int learnedCount = hot_pixel_learner_map(camera, &learnedMap);
	*pixelCount = 0;
	if (learnedCount <= 0 || !learnedMap) {
		return NULL;
	}
	size_t bytes = (size_t)learnedCount * HOT_PIXEL_BYTES;
	u16 *copy = malloc(bytes);
	if (!copy) {
		return NULL;
	}
	memcpy(copy, learnedMap, bytes);
	*pixelCount = learnedCount;
	return copy;
}

// What newlib has not taken from the app's heap yet, plus the free chunks inside what it has. An estimate:
// fragmentation may make the largest block smaller.
static u32 free_heap_bytes(void)
{
	struct mallinfo heapInfo = mallinfo();
	u32 heapSize = envGetHeapSize();
	u32 untaken = heapSize > heapInfo.arena ? heapSize - (u32)heapInfo.arena : 0;
	return untaken + (u32)heapInfo.fordblks;
}

// A raw multi-frame shot is saved like a single raw shot: the press's white balance, the black level for the
// gain and the exposure both sensors used.
static void raw_job_setup(SaveJob *job, const ShotSettings *shot, const RawShotInfo *info)
{
	job->format = CAM_FORMAT_RAW;
	job->width = CAM_FRAME_WIDTH;
	job->height = CAM_FRAME_HEIGHT;
	memcpy(job->whiteBalance, shot->whiteBalance, sizeof job->whiteBalance);
	raw_view_black_levels(job->blackLevels);
	job->exposureSeconds = sensor_exposure_seconds(info->lines);
	job->iso = sensor_exposure_gain_iso(info->gain);
	job->hotFix = shot->hotPixelFix;
	job->photoFormat = shot->settings.photoFormat;
	job->lookIndex = shot->settings.lookIndex;
}

// dual_burst_take's wait limit: the frames plus the few it may wait for, each at least the dual stream's
// frame length, longer when the exposure stretches it.
static u32 burst_timeout_ms(const CamSettings *cam, int frameCount)
{
	float lines = g_exposureNow.valid ? g_exposureNow.lines : 0;
	u16 manualLines;
	u16 manualFine;
	if (sensor_exposure_manual_time(cam, &manualLines, &manualFine) && manualLines > lines) {
		lines = manualLines;
	}
	float exposureFrameLines = lines + SENSOR_EXPOSURE_MARGIN_LINES;
	float frameLines = exposureFrameLines > DUAL_FRAME_LINES ? exposureFrameLines : DUAL_FRAME_LINES;
	float frameMs = frameLines * sensor_exposure_line_microseconds() / (float)MICROSECONDS_PER_MILLISECOND;
	return (u32)((frameCount + BURST_EXTRA_FRAMES) * frameMs) + BURST_TIMEOUT_SLACK_MS;
}

// The DNG's exposure tags and the black level depend on the exposure dual_burst_take reports. When it
// reports none, the last read-back stands in. 0 lines alone is a report: a sub-line time.
static void fill_missing_exposure(RawShotInfo *info)
{
	if (info->gain) {
		return;
	}
	info->gain = g_exposureNow.valid && g_exposureNow.gain ? g_exposureNow.gain : SENSOR_GAIN_CODE_MIN;
	if (!info->lines && g_exposureNow.valid) {
		info->lines = g_exposureNow.lines;
	}
}

// The shutter sounds when the frames are in: from then on the camera may move.
static void play_late_shutter_sound(void)
{
	if (still_capture_take_late_sound()) {
		CAMU_PlayShutterSound(SHUTTER_SOUND_TYPE_NORMAL);
	}
}

// Camera B (the outer left camera) is never on screen, so the live view's hot pixel learner never sees it.
// Its burst frames teach the learner instead: all rows at once, on the camera thread.
static void learn_camera_b_hot_pixels(const u8 *raw)
{
	// Its own scene: the live view's frames must not hold back camera B's bursts.
	static HotPixelScene cameraBScene;
	const bool fastMode = false;
	const bool wholeFrame = true;
	hot_pixel_learner_feed_frame(CAM_OUTER_LEFT, &cameraBScene, raw, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT,
	                             fastMode, RAW10_BLACK, wholeFrame);
}

// The pair from the dual stream's next matched frames when it runs, else the slow way with both sensors set
// up for the shot. A YUV stream takes its pair through the camera service.
static Result take_3d_pair(const CamSettings *cam, bool raw, u8 *const *frameA, u8 *const *frameB,
                           RawShotInfo *info)
{
	if (raw && dual_stream_active()) {
		Result result =
		        dual_burst_take(PAIR_FRAMES, frameA, frameB, info, burst_timeout_ms(cam, PAIR_FRAMES));
		if (result == 0) {
			fill_missing_exposure(info);
			play_late_shutter_sound();
			return result;
		}
		memset(info, 0, sizeof *info);
	}
	if (raw) {
		return still_capture_raw_burst(cam, PAIR_FRAMES, true, frameA, frameB, info);
	}
	return still_capture_yuv_pair(cam, *frameA, *frameB);
}

static const char *pair_note(bool raw, int photoFormat)
{
	if (!raw) {
		return "\n3D pair: L = left eye (camera OUT2), R = right eye (camera OUT1), SBS = both side by side "
		       "(parallel view)\n";
	}
	if (photoFormat == PHOTO_FORMAT_RAW_ONLY) {
		return "\n3D pair (raw): L = left eye (camera OUT2), R = right eye (camera OUT1); DNG = the sensor's "
		       "frame (RAW only: no JPG)\n";
	}
	return "\n3D pair (raw): L = left eye (camera OUT2), R = right eye (camera OUT1); DNG = the sensor's "
	       "frame (a pair always keeps both), JPG = the in-camera look\n";
}

void burst_shot_3d(const ShotSettings *shot)
{
	const CamSettings *cam = &shot->settings.cam;
	u8 *frameA = malloc(CAM_FRAME_BYTES);
	u8 *frameB = malloc(CAM_FRAME_BYTES);
	if (!frameA || !frameB) {
		free(frameA);
		free(frameB);
		toast_show("Not enough memory");
		return;
	}
	bool raw = cam_stream_raw_active();
	RawShotInfo info;
	memset(&info, 0, sizeof info);
	Result result = take_3d_pair(cam, raw, &frameA, &frameB, &info);
	if (R_FAILED(result)) {
		free(frameA);
		free(frameB);
		toast_show("3D capture failed: 0x%08lX", result);
		return;
	}
	review_flash_shutter();
	if (raw) {
		learn_camera_b_hot_pixels(frameB);
	}
	// Camera A is OUT1, the right eye: the pair is saved left eye first.
	SaveJob job = {.kind = SAVE_PAIR,
	               .frameA = frameB,
	               .frameB = frameA,
	               .camSettings = *cam,
	               .exposureBias = cam->exposure,
	               .sizeDivisor = shot_mode_photo_divisor(&shot->settings),
	               .note = strdup(pair_note(raw, shot->settings.photoFormat))};
	if (raw) {
		raw_job_setup(&job, shot, &info);
	}
	saver_next_shot_path(job.path, sizeof job.path, true);
	// The review copies camera A's frame before the saver owns it and fixes its hot pixels.
	review_post(frameA, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, raw ? CAM_FORMAT_RAW : CAM_FORMAT_YUYV,
	            shot->settings.review ? REVIEW_SHOW_MS : 0);
	saver_queue(&job);
	if (!shot->settings.review) {
		toast_show("Saving %s (3D)", g_lastShot);
	}
}

// The HDR times in lines. Camera A gets the metered time x 2^ceil(gap / 2) for the shadows, camera B the
// metered time / 2^floor(gap / 2) for the highlights, so A is 2^gap times B. The ratio holds for the real
// times, each its lines plus the fine part. A metered time too short to divide keeps the gap from B's
// shortest time, the fine part alone: the pair is then brighter than metered.
static void hdr_lines(u32 meteredLines, float fineLines, int gapEv, u16 *linesA, u16 *linesB)
{
	int stepsDown = gapEv / 2;
	float shortLines = floorf((meteredLines + fineLines) / (1 << stepsDown) - fineLines + 0.5f);
	if (shortLines < 0) {
		shortLines = 0;
	}
	// A from B as rounded, so the gap holds.
	float longLines = floorf((shortLines + fineLines) * (1 << gapEv) - fineLines + 0.5f);
	if (gapEv > 0 && longLines <= shortLines) {
		// At the shortest times both would round to 0 lines.
		longLines = shortLines + 1;
	}
	*linesA = (u16)(longLines > SENSOR_LINES_MAX ? SENSOR_LINES_MAX : longLines);
	*linesB = (u16)(shortLines > SENSOR_LINES_MAX ? SENSOR_LINES_MAX : shortLines);
}

// Room for the shot's frames plus MERGE_HEADROOM. The frames of shots already waiting stay in memory until
// their merges, so without it a later step would fail.
static bool merge_memory_available(const BurstPlan *plan)
{
	int cameras = plan->outer ? 2 : 1;
	u32 needed = (u32)plan->frameCount * cameras * CAM_FRAME_BYTES + MERGE_HEADROOM;
	u32 available = free_heap_bytes();
	if (available >= needed) {
		return true;
	}
	toast_show(background_merge_shots_left() > 0 ? "Not enough memory: wait for the merge"
	                                             : "Not enough memory for the merge");
	return false;
}

static bool allocate_frames(MergeRequest *request, const BurstPlan *plan)
{
	for (int i = 0; i < plan->frameCount; i++) {
		request->framesA[i] = malloc(CAM_FRAME_BYTES);
		if (plan->wantB) {
			request->framesB[i] = malloc(CAM_FRAME_BYTES);
		}
		bool allocated = request->framesA[i] && (!plan->wantB || request->framesB[i]);
		if (!allocated) {
			return false;
		}
	}
	return true;
}

// The slow way takes camera B too, into new buffers where the dual stream had none.
static bool allocate_missing_b_frames(MergeRequest *request, int frameCount)
{
	for (int i = 0; i < frameCount; i++) {
		if (request->framesB[i]) {
			continue;
		}
		request->framesB[i] = malloc(CAM_FRAME_BYTES);
		if (!request->framesB[i]) {
			return false;
		}
	}
	return true;
}

// The metered time: the live exposure as the sensor has it, else as last read, else the chosen time. Nothing
// changes it while the camera thread shoots.
static u16 metered_lines(const CamSettings *cam)
{
	SensorExposure exposure;
	if (R_SUCCEEDED(sensor_exposure_read(&exposure)) && exposure.valid) {
		return exposure.lines;
	}
	if (g_exposureNow.valid) {
		return g_exposureNow.lines;
	}
	u16 lines = 0;
	u16 fine;
	sensor_exposure_manual_time(cam, &lines, &fine);
	return lines;
}

static int usable_pairs(Result result, int frameCount, const RawShotInfo *info)
{
	if (result == 0) {
		return frameCount;
	}
	if (result == DUAL_BURST_PARTIAL || result == DUAL_BURST_UNPAIRED) {
		return info->paired;
	}
	return 0;
}

static void swap_frames(u8 **first, u8 **second)
{
	u8 *kept = *first;
	*first = *second;
	*second = kept;
}

// Moves the pairs taken at one moment to the front, in order: the merge takes B_k as A_k's partner.
static int keep_matched_pairs(MergeRequest *request, int frameCount, u32 pairMask)
{
	int kept = 0;
	for (int i = 0; i < frameCount; i++) {
		if (!(pairMask & (1u << i))) {
			continue;
		}
		swap_frames(&request->framesA[kept], &request->framesA[i]);
		swap_frames(&request->framesB[kept], &request->framesB[i]);
		kept++;
	}
	return kept;
}

// HDR: camera A at the long time, camera B at the short one, from the dual stream. Sets request->hdr when the
// pairs are usable, else the note says why.
static Result take_hdr_burst(MergeRequest *request, const BurstPlan *plan, RawShotInfo *info)
{
	const CamSettings *cam = &plan->settings->cam;
	int frameCount = plan->frameCount;
	u16 meteredLines = metered_lines(cam);
	u16 linesA;
	u16 linesB;
	request->hdrGapEv = math_clamp_int(plan->settings->hdrGap, HDR_GAP_MIN_EV, HDR_GAP_MAX_EV);
	request->meteredLines = meteredLines;
	hdr_lines(meteredLines, request->fineLines, request->hdrGapEv, &linesA, &linesB);
	Result result = dual_burst_take_hdr(frameCount, request->framesA, request->framesB, info,
	                                    burst_timeout_ms(cam, frameCount), linesA, linesB);
	if (result == DUAL_BURST_SENSOR_AE) {
		// A sensor's own AE / AWB came back on. The burst asked for the guard, which the retry runs first.
		memset(info, 0, sizeof *info);
		result = dual_burst_take_hdr(frameCount, request->framesA, request->framesB, info,
		                             burst_timeout_ms(cam, frameCount), linesA, linesB);
	}
	int pairs = usable_pairs(result, frameCount, info);
	if (pairs >= 1) {
		if (result != 0) {
			pairs = keep_matched_pairs(request, frameCount, info->pairMask);
		}
		request->hdr = pairs >= 1;
		request->frameCountA = pairs;
		request->frameCountB = pairs;
		request->cameraA = CAM_OUTER_RIGHT;
		request->captureMethod = "dual stream, HDR";
		// The times the sensors got, with their caps applied.
		request->linesA =
		        info->cameraLines[RAW_SHOT_CAMERA_A] ? info->cameraLines[RAW_SHOT_CAMERA_A] : linesA;
		request->linesB =
		        info->cameraLines[RAW_SHOT_CAMERA_B] ? info->cameraLines[RAW_SHOT_CAMERA_B] : linesB;
		if (request->linesA <= request->linesB) {
			snprintf(request->note, sizeof request->note,
			         "HDR: camera A's time was capped at camera B's (%u lines): no exposure gap, "
			         "the extra range is only camera B's lower sensitivity",
			         request->linesB);
		}
		play_late_shutter_sound();
		fill_missing_exposure(info);
	}
	if (!request->hdr) {
		snprintf(request->note, sizeof request->note,
		         "HDR burst failed (%08lX, %d of %d pairs from one moment): a plain merge at the "
		         "metered exposure",
		         (unsigned long)result, info->paired, frameCount);
		memset(info, 0, sizeof *info);
	}
	return result;
}

// The slow way: both sensors set up for the shot.
static BurstOutcome take_reconfigured_burst(MergeRequest *request, const BurstPlan *plan, RawShotInfo *info,
                                            Result *result)
{
	int frameCount = plan->frameCount;
	memset(info, 0, sizeof *info);
	if (plan->outer && !allocate_missing_b_frames(request, frameCount)) {
		return BURST_NO_MEMORY;
	}
	u8 *const *framesB = plan->outer ? request->framesB : NULL;
	*result = still_capture_raw_burst(&plan->settings->cam, frameCount, plan->outer, request->framesA,
	                                  framesB, info);
	request->frameCountA = frameCount;
	request->frameCountB = plan->outer ? frameCount : 0;
	request->cameraA = plan->outer ? CAM_OUTER_RIGHT : CAM_INNER;
	request->captureMethod = "reconfigured burst";
	return R_SUCCEEDED(*result) ? BURST_CAPTURED : BURST_FAILED;
}

// MERGE, or HDR's fallback: one exposure, the metered one. The next frames of the running stream, merged
// even when the burst came out short; else the slow way.
static BurstOutcome take_plain_burst(MergeRequest *request, const BurstPlan *plan, RawShotInfo *info,
                                     Result *result)
{
	const CamSettings *cam = &plan->settings->cam;
	int frameCount = plan->frameCount;
	u8 *const *framesB = plan->wantB ? request->framesB : NULL;
	*result = dual_burst_take(frameCount, request->framesA, framesB, info, burst_timeout_ms(cam, frameCount));
	bool shortBurst =
	        *result == DUAL_BURST_PARTIAL && info->received[RAW_SHOT_CAMERA_A] >= PARTIAL_BURST_MIN_FRAMES;
	if (*result != 0 && !shortBurst) {
		return take_reconfigured_burst(request, plan, info, result);
	}
	if (*result == 0) {
		request->frameCountA = frameCount;
	} else {
		request->frameCountA = math_clamp_int(info->received[RAW_SHOT_CAMERA_A], 1, frameCount);
	}
	request->frameCountB = plan->wantB ? math_clamp_int(info->received[RAW_SHOT_CAMERA_B], 0, frameCount) : 0;
	// A single stream's camera A is the camera streaming.
	request->cameraA = plan->dual ? CAM_OUTER_RIGHT : cam->camera;
	request->captureMethod = plan->dual ? "dual stream" : "single stream";
	play_late_shutter_sound();
	fill_missing_exposure(info);
	return BURST_CAPTURED;
}

// HDR pairs, or one exposure. MERGE and HDR never run the 60 fps view, but a single stream left in it would
// refuse dual_burst_take: it is switched off for the burst and back after.
static BurstOutcome capture_merge_frames(MergeRequest *request, const BurstPlan *plan, bool hdr,
                                         RawShotInfo *info, Result *result)
{
	bool fastBack = !plan->dual && fast_view_active();
	if (fastBack) {
		fast_view_switch(false);
	}
	memset(info, 0, sizeof *info);
	u64 startMs = osGetTime();
	*result = -1;
	if (hdr && !plan->wantB) {
		snprintf(request->note, sizeof request->note,
		         "HDR asked for, but the outer cameras were not streaming together (the dual stream did "
		         "not start, or was unstable): a plain merge at the metered exposure");
	} else if (hdr) {
		*result = take_hdr_burst(request, plan, info);
	}
	BurstOutcome outcome = BURST_CAPTURED;
	if (!request->hdr) {
		outcome = take_plain_burst(request, plan, info, result);
	}
	if (outcome == BURST_NO_MEMORY) {
		return outcome;
	}
	request->captureMs = (u32)(osGetTime() - startMs);
	// A restart of the stream by the slow way may have brought the fast view back already.
	if (fastBack && cam_stream_raw_active() && !fast_view_active()) {
		fast_view_switch(true);
	}
	return outcome;
}

// A short burst's empty slots are not carried through the queue and the merge.
static void free_unused_frames(MergeRequest *request, int frameCount)
{
	for (int i = 0; i < frameCount; i++) {
		if (i >= request->frameCountA) {
			free(request->framesA[i]);
			request->framesA[i] = NULL;
		}
		if (i >= request->frameCountB) {
			free(request->framesB[i]);
			request->framesB[i] = NULL;
		}
	}
}

static void fill_request_exposure(MergeRequest *request, const ShotSettings *shot, const RawShotInfo *info)
{
	const CamSettings *cam = &shot->settings.cam;
	request->packed = true;
	request->info = *info;
	request->camSettings = *cam;
	request->exposureBias = cam->exposure;
	request->gain = math_clamp_int(info->gain, SENSOR_GAIN_CODE_MIN, MERGE_GAIN_CODE_MAX);
	request->iso = sensor_exposure_gain_iso(info->gain);
	// HDR: camera A's time, the longer one.
	request->exposureSeconds = sensor_exposure_seconds(info->lines);
	memcpy(request->whiteBalance, shot->whiteBalance, sizeof request->whiteBalance);
	raw_view_black_levels(request->blackLevelsA);
	// The same gain on both cameras. The merge fits camera B's darker response.
	memcpy(request->blackLevelsB, request->blackLevelsA, sizeof request->blackLevelsB);
}

// The review is a copy taken before the merge unpacks the frames in place. HDR shows camera B's frame scaled
// to the metered exposure: camera A's longer one would show every HDR shot blown out. Tagged MERGING until
// the merge has run.
static void post_merge_review(const MergeRequest *request)
{
	if (request->hdr && request->framesB[0]) {
		float meteredTime = request->meteredLines + request->fineLines;
		float timeB = request->linesB + request->fineLines;
		float brightness = meteredTime / timeB * MERGE_HDR_SENSITIVITY;
		review_post_scaled(request->framesB[0], CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, CAM_FORMAT_RAW, 0,
		                   brightness, request->blackLevelsB, "MERGING");
		return;
	}
	review_post_scaled(request->framesA[0], CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, CAM_FORMAT_RAW, 0, 1, NULL,
	                   "MERGING");
}

// The merge applies the defect maps to every frame.
static void attach_hot_pixel_maps(MergeRequest *request)
{
	request->hotPixelsA = copy_hot_pixel_map(request->cameraA, &request->hotPixelCountA);
	if (request->frameCountB) {
		request->hotPixelsB = copy_hot_pixel_map(CAM_OUTER_LEFT, &request->hotPixelCountB);
	}
}

// MERGE and HDR: a burst from each outer camera, or camera A alone with the inner one, merged into one raw
// photo in the background while the camera carries on. HDR needs both cameras at one moment. Without the dual
// stream, or when its burst fails, the shot is a plain merge at the metered exposure, and its TXT and toast
// say why.
void burst_shot_merge(const ShotSettings *shot, bool hdr)
{
	const Settings *settings = &shot->settings;
	if (background_merge_queue_full()) {
		toast_show("Still merging: wait a moment");
		return;
	}
	BurstPlan plan;
	plan.settings = settings;
	plan.frameCount = math_clamp_int(shot_mode_merge_frames(settings), 1, MERGE_MAX_FRAMES);
	plan.outer = settings->cam.camera != CAM_INNER;
	plan.dual = dual_stream_active();
	// A single stream gives camera A alone: unused camera B frames would ride through the queue and the
	// merge.
	plan.wantB = plan.outer && plan.dual;
	if (!merge_memory_available(&plan)) {
		return;
	}
	MergeRequest request;
	memset(&request, 0, sizeof request);
	request.hdrWanted = hdr;
	// The camera thread shoots and also runs cam_stream_apply, so this stays valid for the burst.
	request.fineLines = sensor_exposure_fine_lines();
	if (!allocate_frames(&request, &plan)) {
		background_merge_free_request(&request);
		toast_show("Not enough memory for the merge");
		return;
	}
	RawShotInfo info;
	Result result;
	BurstOutcome outcome = capture_merge_frames(&request, &plan, hdr, &info, &result);
	if (outcome == BURST_NO_MEMORY) {
		background_merge_free_request(&request);
		toast_show("Not enough memory for the merge");
		return;
	}
	if (outcome == BURST_FAILED) {
		background_merge_free_request(&request);
		toast_show("Merge capture failed: 0x%08lX", result);
		return;
	}
	free_unused_frames(&request, plan.frameCount);
	review_flash_shutter();
	if (request.frameCountB > 0 && request.framesB[0]) {
		learn_camera_b_hot_pixels(request.framesB[0]);
	}
	// Only the name: the saver creates the files.
	saver_next_shot_path(request.basePath, sizeof request.basePath, true);
	request.id = shot_names_number_of_path(request.basePath);
	fill_request_exposure(&request, shot, &info);
	post_merge_review(&request);
	request.lookIndex = settings->lookIndex;
	request.photoFormat = settings->photoFormat;
	request.sizeDivisor = shot_mode_photo_divisor(settings);
	if (shot->hotPixelFix) {
		attach_hot_pixel_maps(&request);
	}
	request.when = time(NULL);
	if (!background_merge_push(&request)) {
		background_merge_free_request(&request);
		toast_show("Still merging: wait a moment");
		return;
	}
	if (settings->review) {
		// Only now: a shot the queue refused is not shown as taken.
		review_request(REVIEW_SHOW_MS);
	}
	toast_show("%s: %s%d+%d frames in %.2f s, merging", g_lastShot, request.hdr ? "HDR " : "",
	           request.frameCountA, request.frameCountB, request.captureMs / (double)MILLISECONDS_PER_SECOND);
}
