#include "still_shot.h"
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "auto_exposure.h"
#include "cam_settings.h"
#include "cam_stream.h"
#include "fast_view.h"
#include "math_util.h"
#include "photo_metadata.h"
#include "photo_writer.h"
#include "raw_view.h"
#include "review.h"
#include "saver.h"
#include "sensor_exposure.h"
#include "settings.h"
#include "shot_info.h"
#include "shot_mode.h"
#include "shot_names.h"
#include "still_capture.h"
#include "time_units.h"
#include "toast.h"

#define SETTLE_TIMEOUT_MS 2000 // plus the exposure time of the frames waited for
#define SETTLE_POLL_NS (4 * NANOSECONDS_PER_MILLISECOND)
#define RAW_FRAME_TIMEOUT_MS 3000
#define RAW_FRAME_POLL_NS (2 * NANOSECONDS_PER_MILLISECOND)
#define START_WINDOW_TIMEOUT_MS 600 // the longest wait for a stream start's window to close
#define START_WINDOW_POLL_NS (2 * NANOSECONDS_PER_MILLISECOND)

// Frames to wait after a new exposure. On the sensor the second frame after a write is the first entirely
// at the new exposure; the third makes sure.
#define RAW_SETTLE_FRAMES 3
#define YUV_SETTLE_FRAMES 10
#define YUV_PHOTO_SKIP_FRAMES 2 // frames the camera service drops while the still settles

// BRACKET's series (Settings.bracketSeries): the EV steps from the metered exposure, in the order they are
// taken.
#define BRACKET_MAX_FRAMES 5

typedef struct {
	int frameCount;
	s8 steps[BRACKET_MAX_FRAMES];
} BracketSeries;

static const BracketSeries BRACKET_SERIES[BRACKET_SERIES_CHOICE_COUNT] = {
        {3, {-1, 0, 1}},
        {3, {-2, 0, 2}},
        {5, {-2, -1, 0, 1, 2}},
};

// A raw bracket step whose rounded lines miss it by more than this tries the neighbouring whole lines.
#define MAX_STEP_MISS_EV (1 / 6.0f)

#define NO_EV_OFFSET 99 // no bracket frame named yet: outside every offset a series takes
#define NOTE_MAX 160

// One photo's frame as captured.
typedef struct {
	u8 *pixels; // malloc'd: the save job takes it
	int width;
	int height;
	int format; // CAM_FORMAT_*
	SensorExposure exposure; // raw: as read back from the sensor
} StillFrame;

// A shot in progress: one photo, or a bracket's series in one folder.
typedef struct {
	const ShotSettings *shot;
	CamSettings base; // the settings before the shot
	bool raw;
	SensorExposure metered; // a raw bracket's steps are relative to it
	const BracketSeries *series;
	const char *seriesName;
	int frameCount; // 1: a single photo
	char folder[SHOT_PATH_MAX]; // a bracket's folder
	int previousOffset; // the EV offset of the frame before, for the file name
} StillShot;

// Queue one photo for saving. `settings` are the frame's own and `path` names a bracket frame (NULL: the next
// shot name). `note` (may be NULL) is a line for the sidecar.
static void save_still(const ShotSettings *shot, const CamSettings *settings, int exposureBias,
                       const char *path, const StillFrame *frame, const char *note)
{
	SaveJob job = {.kind = SAVE_FRAME,
	               .frameA = frame->pixels,
	               .format = frame->format,
	               .width = CAM_FRAME_WIDTH,
	               .height = CAM_FRAME_HEIGHT,
	               .camSettings = *settings,
	               .exposureBias = exposureBias,
	               .sizeDivisor = shot_mode_photo_divisor(&shot->settings),
	               .note = note ? strdup(note) : NULL};
	if (frame->format == CAM_FORMAT_RAW) {
		const SensorExposure *exposure = &frame->exposure;
		job.width = frame->width;
		job.height = frame->height;
		memcpy(job.whiteBalance, shot->whiteBalance, sizeof job.whiteBalance);
		job.exposureSeconds = exposure->valid ? exposure->seconds : 0;
		raw_view_black_levels(job.blackLevels);
		job.hotFix = shot->hotPixelFix;
		job.photoFormat = shot->settings.photoFormat;
		job.lookIndex = shot->settings.lookIndex;
		job.iso = exposure->valid ? exposure->iso : 0;
	}
	if (path) {
		snprintf(job.path, sizeof job.path, "%.*s", (int)sizeof job.path - 1, path);
	} else {
		saver_next_shot_path(job.path, sizeof job.path, false);
	}
	// The review gets its copy before the saver fixes hot pixels in the frame. It shows at the end of the
	// shot.
	review_post(frame->pixels, frame->width, frame->height, frame->format, 0);
	saver_queue(&job);
}

// Wait for a number of fresh frames, so the exposure settles on a new bias or time. Long exposure times
// stretch the frames, so the timeout grows with `lines`.
static void settle(int frames, u32 lines)
{
	u64 start = osGetTime();
	float exposureMs =
	        frames * lines * sensor_exposure_line_microseconds() / (float)MICROSECONDS_PER_MILLISECOND;
	u64 timeout = SETTLE_TIMEOUT_MS + (u64)exposureMs;
	u16 *pixels;
	int width;
	int height;
	while (frames > 0 && osGetTime() - start < timeout) {
		if (cam_stream_poll(&pixels, &width, &height)) {
			frames--;
		} else {
			svcSleepThread(SETTLE_POLL_NS);
		}
	}
}

// Copy the raw stream's frame after `skip` more into destination, with its size and the exposure read back
// right after it. False and nothing written when no frame comes in time.
static bool raw_next(u8 *destination, int *width, int *height, SensorExposure *exposure, int skip)
{
	u64 start = osGetTime();
	u16 *pixels;
	int frameWidth;
	int frameHeight;
	while (osGetTime() - start < RAW_FRAME_TIMEOUT_MS) {
		if (cam_stream_poll(&pixels, &frameWidth, &frameHeight)) {
			if (skip <= 0) {
				memcpy(destination, pixels, (size_t)frameWidth * frameHeight * CAM_PIXEL_BYTES);
				*width = frameWidth;
				*height = frameHeight;
				sensor_exposure_read(exposure);
				return true;
			}
			skip--;
		}
		svcSleepThread(RAW_FRAME_POLL_NS);
	}
	return false;
}

// The sensor's own AE / AWB ran for the frame in destination: the module may switch them back on after a
// start. Put Pro Cam's exposure back once the start's window has passed, let it settle and take the frame
// again.
static void retake_with_own_exposure(u8 *destination, int *width, int *height, SensorExposure *exposure)
{
	u64 start = osGetTime();
	while (sensor_exposure_unverified() && osGetTime() - start < START_WINDOW_TIMEOUT_MS) {
		svcSleepThread(START_WINDOW_POLL_NS);
	}
	sensor_exposure_guard();
	// Wait long enough for the longer of the refused time and the one the guard put back.
	SensorExposure restored;
	bool readBack = R_SUCCEEDED(sensor_exposure_read(&restored)) && restored.valid;
	u32 settleLines = exposure->lines;
	if (readBack && restored.lines > exposure->lines) {
		settleLines = restored.lines;
	}
	settle(RAW_SETTLE_FRAMES, settleLines);
	raw_next(destination, width, height, exposure, 0);
}

// Raw mode: the photo is the next full frame of the raw stream, CAM_FRAME_BYTES into destination. The fast
// live view skips rows, so it is left for the shot and comes back after it.
static bool raw_take(u8 *destination, int *width, int *height, SensorExposure *exposure)
{
	bool fastView = fast_view_active();
	if (fastView) {
		fast_view_switch(false);
	}
	// The first frame after the switch began exposing in the fast mode.
	int framesToDrop = fastView ? 1 : 0;
	bool ok = raw_next(destination, width, height, exposure, framesToDrop);
	if (ok && exposure->foreign) {
		retake_with_own_exposure(destination, width, height, exposure);
	}
	if (fastView) {
		fast_view_switch(true);
	}
	return ok;
}

// How far whole lines plus the fine part at a gain code are from the target exposure, in EV.
static float exposure_miss_ev(float target, int lines, int gainCode, float fineLines)
{
	return fabsf(log2f((lines + fineLines) * sensor_exposure_gain_factor(gainCode) / target));
}

// The lines, fine part excluded, that reach the target exposure at a gain code.
static float lines_at_gain(float target, int gainCode, float fineLines)
{
	return target / sensor_exposure_gain_factor(gainCode) - fineLines;
}

// A raw bracket frame's exposure: the metered one times 2^step. The time moves first, at the metered gain,
// up to auto_exposure_max_lines; beyond that the gain rises. Below the fine part alone (0 lines) the gain
// comes down. What cannot be reached stays at the limit. At a few lines one whole line is a big step, so a
// rounded time that misses by more than MAX_STEP_MISS_EV takes the floor or ceiling of the lines instead,
// with the gain that makes up the rest, whichever is nearer.
static void bracket_step_exposure(const SensorExposure *metered, int step, u16 *lines, u16 *gain)
{
	int meteredGain = sensor_exposure_even_gain_code(metered->gain);
	*lines = metered->lines;
	*gain = (u16)meteredGain;
	if (step == 0) {
		return; // computing the metered frame again in float can round it a gain step down
	}
	float fineLines = sensor_exposure_fine_lines();
	float minLines = 0; // the fine part alone, a sub-line time
	float maxLines = auto_exposure_max_lines();
	float stepFactor = powf(2.0f, (float)step);
	float target = (metered->lines + fineLines) * sensor_exposure_gain_factor(meteredGain) * stepFactor;
	int gainCode = meteredGain;
	float exactLines = lines_at_gain(target, gainCode, fineLines);
	if (exactLines > maxLines) {
		gainCode = sensor_exposure_gain_code(target / (maxLines + fineLines), GAIN_ROUND_UP);
		exactLines = lines_at_gain(target, gainCode, fineLines);
	} else if (exactLines < minLines) {
		gainCode = sensor_exposure_gain_code(target / (minLines + fineLines), GAIN_ROUND_DOWN);
		exactLines = lines_at_gain(target, gainCode, fineLines);
	}
	if (exactLines > maxLines) {
		exactLines = maxLines;
	} else if (exactLines < minLines) {
		exactLines = minLines;
	}
	int wholeLines = (int)(exactLines + 0.5f);
	float bestMiss = exposure_miss_ev(target, wholeLines, gainCode, fineLines);
	if (bestMiss > MAX_STEP_MISS_EV) {
		int candidates[] = {(int)floorf(exactLines), (int)ceilf(exactLines)};
		for (int i = 0; i < ARRAY_COUNT(candidates); i++) {
			int candidateLines = candidates[i];
			if (candidateLines < minLines || candidateLines > maxLines) {
				continue;
			}
			int candidateGain =
			        sensor_exposure_gain_code(target / (candidateLines + fineLines), GAIN_ROUND_NEAREST);
			float miss = exposure_miss_ev(target, candidateLines, candidateGain, fineLines);
			if (miss < bestMiss) {
				bestMiss = miss;
				wholeLines = candidateLines;
				gainCode = candidateGain;
			}
		}
	}
	*lines = (u16)wholeLines;
	*gain = (u16)gainCode;
}

// The exposure a raw bracket's steps are relative to. False when it cannot be read.
static bool read_metered_exposure(SensorExposure *metered)
{
	if (R_FAILED(sensor_exposure_read(metered))) {
		return false;
	}
	return metered->valid && metered->gain != 0;
}

// One photo, or a bracket's series. A raw bracket needs the metered exposure to step from.
static int frames_to_take(bool series, const BracketSeries *bracketSeries, bool raw, SensorExposure *metered)
{
	int frameCount = series ? bracketSeries->frameCount : 1;
	if (raw && frameCount > 1 && !read_metered_exposure(metered)) {
		toast_show("Bracket: the exposure could not be read, one photo");
		return 1;
	}
	return frameCount;
}

// Give one bracket frame its exposure and wait until the frames show it. Raw: the step goes to the sensor,
// relative to the metered exposure, so it is not held to the EV dial's range. YUV: the step is already in
// frameSettings as the camera's EV bias.
static void apply_bracket_step(bool raw, const SensorExposure *metered, int step,
                               const CamSettings *frameSettings)
{
	if (!raw) {
		cam_stream_apply(frameSettings);
		settle(YUV_SETTLE_FRAMES, 0);
		return;
	}
	u16 lines;
	u16 gain;
	bracket_step_exposure(metered, step, &lines, &gain);
	sensor_exposure_set_lines(lines);
	sensor_exposure_set_gain(gain);
	settle(RAW_SETTLE_FRAMES, lines);
}

// Capture one photo's frame. False, after telling the user why, when there is none.
static bool take_frame(bool raw, const CamSettings *frameSettings, StillFrame *frame)
{
	frame->pixels = malloc(CAM_FRAME_BYTES);
	if (!frame->pixels) {
		toast_show("Not enough memory to save");
		return false;
	}
	frame->width = CAM_FRAME_WIDTH;
	frame->height = CAM_FRAME_HEIGHT;
	frame->format = frameSettings->format;
	memset(&frame->exposure, 0, sizeof frame->exposure);
	if (raw) {
		frame->format = CAM_FORMAT_RAW;
		if (!raw_take(frame->pixels, &frame->width, &frame->height, &frame->exposure)) {
			free(frame->pixels);
			toast_show("No raw frame from the camera");
			return false;
		}
		return true;
	}
	u16 *pixels = (u16 *)(void *)frame->pixels;
	Result result = still_capture_yuv_photo(pixels, YUV_PHOTO_SKIP_FRAMES);
	if (R_FAILED(result)) {
		free(frame->pixels);
		toast_show("Capture failed: 0x%08lX", result);
		return false;
	}
	return true;
}

// A bracket frame's file, named by its EV offset from the metered exposure. A YUV step cut short at the end
// of the EV range repeats its neighbour's offset, so that frame also gets its number.
static void bracket_frame_path(char *path, size_t size, const char *folder, int offset, int previousOffset,
                               int frameNumber)
{
	if (offset == previousOffset) {
		snprintf(path, size, "%s/EV%+d_%d.PNG", folder, offset, frameNumber);
	} else {
		snprintf(path, size, "%s/EV%+d.PNG", folder, offset);
	}
}

// Back to the metered exposure after a bracket; the auto exposure goes on from it.
static void restore_metered_exposure(bool raw, const SensorExposure *metered, const CamSettings *base)
{
	if (raw) {
		sensor_exposure_set_lines(metered->lines);
		sensor_exposure_set_gain(metered->gain);
	} else {
		cam_stream_apply(base);
	}
}

// The bracket folder's INFO.TXT, which tells the gallery what the folder holds.
static void queue_bracket_info(const ShotSettings *shot, const CamSettings *base, bool raw, int frames,
                               const char *folder)
{
	const float *rawWhiteBalance = raw ? shot->whiteBalance : NULL;
	char *sidecar = photo_metadata_sidecar_text(base, base->exposure, NULL, rawWhiteBalance);
	bool stereo = false;
	bool hdr = false;
	SaveJob job = {.kind = SAVE_TEXT,
	               .note = shot_info_text("bracket", frames, stereo, hdr, base->exposure, base->exposure,
	                                      base, sidecar)};
	free(sidecar);
	int folderRoom = (int)(sizeof job.path - sizeof "/INFO.TXT");
	snprintf(job.path, sizeof job.path, "%.*s/INFO.TXT", folderRoom, folder);
	saver_queue(&job);
}

// The sidecar line of a bracket frame.
static void bracket_frame_note(const StillShot *still, int frameNumber, int step, int exposureBias,
                               char *note, size_t size)
{
	if (still->raw) {
		snprintf(note, size, "bracket: %s, frame %d of %d, %+d EV from the metered %u lines x gain %u\n",
		         still->seriesName, frameNumber, still->frameCount, step, still->metered.lines,
		         still->metered.gain);
	} else {
		snprintf(note, size, "bracket: %s, frame %d of %d, exposure bias %+d EV\n", still->seriesName,
		         frameNumber, still->frameCount, exposureBias);
	}
}

// Frame `index` of the shot: its exposure, its capture and its save job. False when no frame came.
static bool take_shot_frame(StillShot *still, int index)
{
	bool bracket = still->frameCount > 1;
	int frameNumber = index + 1;
	CamSettings frameSettings = still->base;
	int step = bracket ? still->series->steps[index] : 0;
	int exposureBias = still->base.exposure + step;
	if (!still->raw) {
		exposureBias = math_clamp_int(exposureBias, -CAM_EXPOSURE_BIAS_MAX, CAM_EXPOSURE_BIAS_MAX);
	}
	if (bracket) {
		if (!still->raw) {
			frameSettings.exposure = exposureBias;
		}
		apply_bracket_step(still->raw, &still->metered, step, &frameSettings);
	}
	StillFrame frame;
	if (!take_frame(still->raw, &frameSettings, &frame)) {
		return false;
	}
	review_flash_shutter();
	char path[SHOT_FILE_PATH_MAX];
	char note[NOTE_MAX] = "";
	if (bracket) {
		int offset = exposureBias - still->base.exposure;
		bracket_frame_path(path, sizeof path, still->folder, offset, still->previousOffset, frameNumber);
		still->previousOffset = offset;
		bracket_frame_note(still, frameNumber, step, exposureBias, note, sizeof note);
	}
	const char *framePath = bracket ? path : NULL;
	const char *frameNote = note[0] ? note : NULL;
	save_still(still->shot, &frameSettings, exposureBias, framePath, &frame, frameNote);
	return true;
}

void still_shot_take(const ShotSettings *shot, bool series)
{
	const Settings *settings = &shot->settings;
	int seriesIndex = math_clamp_int(settings->bracketSeries, 0, BRACKET_SERIES_CHOICE_COUNT - 1);
	StillShot still = {.shot = shot,
	                   .base = settings->cam,
	                   .raw = cam_stream_raw_active(),
	                   .series = &BRACKET_SERIES[seriesIndex],
	                   .seriesName = BRACKET_SERIES_NAMES[seriesIndex],
	                   .previousOffset = NO_EV_OFFSET};
	still.frameCount = frames_to_take(series, still.series, still.raw, &still.metered);
	bool bracket = still.frameCount > 1;
	// A bracket stays in full frames throughout instead of leaving the fast view for every frame.
	bool fastViewBack = still.raw && bracket && fast_view_active();
	if (fastViewBack) {
		fast_view_switch(false);
	}
	if (bracket) {
		saver_next_shot_path(still.folder, sizeof still.folder, true);
	}
	int saved = 0;
	while (saved < still.frameCount && take_shot_frame(&still, saved)) {
		saved++;
	}
	if (bracket) {
		restore_metered_exposure(still.raw, &still.metered, &still.base);
		// A series whose first frame failed leaves no folder, and its number stays unused.
		if (saved > 0) {
			queue_bracket_info(shot, &still.base, still.raw, saved, still.folder);
		}
	}
	if (fastViewBack && cam_stream_raw_active() && !fast_view_active()) {
		fast_view_switch(true);
	}
	if (saved == 0) {
		return;
	}
	if (settings->review) {
		review_request(REVIEW_SHOW_MS); // the last frame: a bracket's frames show only at the end
	} else {
		toast_show("Saving %s%s", g_lastShot, saved > 1 ? " (bracket)" : "");
	}
}
