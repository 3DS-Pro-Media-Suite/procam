#include "background_merge.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_threads.h"
#include "cam_stream.h"
#include "gallery.h"
#include "merge.h"
#include "merge_report.h"
#include "photo_writer.h"
#include "review.h"
#include "saver.h"
#include "settings.h"
#include "shot_names.h"
#include "toast.h"

// One merge runs at a time, on its own thread with a helper on another core. The camera thread queues shots,
// the main loop starts the next merge when none runs and hands the result to the saver. The shutter never
// waits for a merge.

#define MERGE_QUEUE_SIZE 2
#define MERGE_LOG_SIZE 2048
#define MERGE_STACK_SIZE (64 * 1024)
#define HELPER_STACK_SIZE (16 * 1024) // a helper needs under 8 KB
#define MERGE_THREADS_WANTED 2 // the merge's own thread and one helper

// Thread priorities below the main thread's. Threads of one priority are not time-sliced and a merge never
// blocks, so the merge stays below every thread that must keep running beside it.
#define MERGE_STEPS_BELOW_MAIN 1 // New 3DS: below the main thread and the preview worker on core 2
#define MERGE_STEPS_BELOW_MAIN_OLD3DS 2 // below the threads at main + 1 that share its core
#define HELPER_STEPS_BELOW_MAIN 2 // below the camera threads, the saver and the gallery loader

#define HELPER_CORE_NEW3DS 0 // beside the UI; the merge itself is on core 2
#define HELPER_CORE_OLD3DS 1 // the system core
#define CORE1_APP_PERCENT 30 // the app's share of the system core, as libctru's examples use

enum {
	CORE1_SHARE_REFUSED = -1,
	CORE1_SHARE_NOT_ASKED = 0,
	CORE1_SHARE_GRANTED = 1,
};

typedef struct {
	MergeRequest requests[MERGE_QUEUE_SIZE];
	int head;
	_Atomic int count;
	LightLock lock;
} MergeQueue;

// The merge running now. It owns its request until background_merge_poll hands the result to the saver.
typedef struct {
	bool active;
	_Atomic bool done;
	bool succeeded;
	Thread thread;
	int core;
	int shotId;
	_Atomic int progress;
	char log[MERGE_LOG_SIZE];
	u64 startedMs;
	MergeRequest request;
	MergeJob job;
	MergeResult result;
	int threadCount; // threads asked of merge_run, its own included; set before its thread starts
	int helperCore;
	int helperPriority;
} MergeSlot;

static MergeQueue g_mergeQueue;
static MergeSlot g_mergeSlot;

void background_merge_init(void)
{
	LightLock_Init(&g_mergeQueue.lock);
}

// An Old 3DS has memory for one waiting shot only.
static int queue_limit(void)
{
	return g_isNew3ds ? MERGE_QUEUE_SIZE : 1;
}

// Only the camera thread adds shots, so room found here is still there at background_merge_push.
bool background_merge_queue_full(void)
{
	return g_mergeQueue.count >= queue_limit();
}

void background_merge_free_request(MergeRequest *request)
{
	for (int i = 0; i < MERGE_MAX_FRAMES; i++) {
		free(request->framesA[i]);
		free(request->framesB[i]);
		request->framesB[i] = NULL;
		request->framesA[i] = NULL;
	}
	free(request->hotPixelsA);
	free(request->hotPixelsB);
	request->hotPixelsB = NULL;
	request->hotPixelsA = NULL;
}

bool background_merge_push(const MergeRequest *request)
{
	LightLock_Lock(&g_mergeQueue.lock);
	bool room = g_mergeQueue.count < MERGE_QUEUE_SIZE;
	if (room) {
		int tail = (g_mergeQueue.head + g_mergeQueue.count) % MERGE_QUEUE_SIZE;
		g_mergeQueue.requests[tail] = *request;
		g_mergeQueue.count++;
	}
	LightLock_Unlock(&g_mergeQueue.lock);
	return room;
}

static bool queue_pop(MergeRequest *request)
{
	LightLock_Lock(&g_mergeQueue.lock);
	bool have = g_mergeQueue.count > 0;
	if (have) {
		*request = g_mergeQueue.requests[g_mergeQueue.head];
		g_mergeQueue.head = (g_mergeQueue.head + 1) % MERGE_QUEUE_SIZE;
		g_mergeQueue.count--;
	}
	LightLock_Unlock(&g_mergeQueue.lock);
	return have;
}

static bool queue_holds_shot(int id)
{
	LightLock_Lock(&g_mergeQueue.lock);
	bool holds = false;
	for (int i = 0; i < g_mergeQueue.count && !holds; i++) {
		int slot = (g_mergeQueue.head + i) % MERGE_QUEUE_SIZE;
		holds = g_mergeQueue.requests[slot].id == id;
	}
	LightLock_Unlock(&g_mergeQueue.lock);
	return holds;
}

bool background_merge_shot_busy(int id)
{
	return (g_mergeSlot.active && g_mergeSlot.shotId == id) || queue_holds_shot(id);
}

// FTP session threads ask this: DELE and RMD wait while the shot may still get files.
bool background_merge_path_busy(const char *path)
{
	int id = shot_names_number_of_path(path);
	return id >= 0 && (background_merge_shot_busy(id) || saver_shot_saving(id));
}

static uint64_t merge_clock_microseconds(void)
{
	return (uint64_t)(svcGetSystemTick() / CPU_TICKS_PER_USEC);
}

// The merge hands out small pieces to whichever thread asks next, so a helper on a busy core just takes
// fewer, and the photo is the same bit for bit. NULL when the thread cannot start: merge_run then does that
// share itself.
static void *spawn_merge_helper(void (*entry)(void *arg), void *arg, int worker)
{
	return threadCreate(entry, arg, HELPER_STACK_SIZE, g_mergeSlot.helperPriority, g_mergeSlot.helperCore,
	                    false);
}

static void join_merge_helper(void *handle)
{
	threadJoin((Thread)handle, U64_MAX);
	threadFree((Thread)handle);
}

// Old 3DS: the helper can run on the system core once the app has a share of it. Asked once and kept: the
// system modules' threads are above ours there anyway.
static bool core1_share(void)
{
	static int shareState = CORE1_SHARE_NOT_ASKED;
	if (shareState != CORE1_SHARE_NOT_ASKED) {
		return shareState == CORE1_SHARE_GRANTED;
	}
	u32 percent = 0;
	Result result = APT_GetAppCpuTimeLimit(&percent);
	if (R_SUCCEEDED(result) && percent < CORE1_APP_PERCENT) {
		result = APT_SetAppCpuTimeLimit(CORE1_APP_PERCENT);
	}
	u32 grantedPercent = 0;
	if (R_SUCCEEDED(result)) {
		APT_GetAppCpuTimeLimit(&grantedPercent);
	}
	bool granted = R_SUCCEEDED(result) && grantedPercent > 0;
	shareState = granted ? CORE1_SHARE_GRANTED : CORE1_SHARE_REFUSED;
	return granted;
}

static void merge_thread(void *arg)
{
	(void)arg;
	MergeRequest *request = &g_mergeSlot.request;
	MergeJob *job = &g_mergeSlot.job;
	memset(job, 0, sizeof *job);
	memset(&g_mergeSlot.result, 0, sizeof g_mergeSlot.result);
	for (int i = 0; i < request->frameCountA; i++) {
		job->framesA[i] = request->framesA[i];
	}
	for (int i = 0; i < request->frameCountB; i++) {
		job->framesB[i] = request->framesB[i];
	}
	job->frameCountA = request->frameCountA;
	job->frameCountB = request->frameCountB;
	job->packed = request->packed;
	memcpy(job->blackA, request->blackLevelsA, sizeof job->blackA);
	memcpy(job->blackB, request->blackLevelsB, sizeof job->blackB);
	job->gain = request->gain;
	job->hotA = request->hotPixelsA;
	job->hotCountA = request->hotPixelCountA;
	job->hotB = request->hotPixelsB;
	job->hotCountB = request->hotPixelCountB;
	job->progress = &g_mergeSlot.progress;
	job->log = g_mergeSlot.log;
	job->logCapacity = sizeof g_mergeSlot.log;
	job->clockMicroseconds = merge_clock_microseconds;
	job->threads = g_mergeSlot.threadCount;
	job->spawn = g_mergeSlot.threadCount > 1 ? spawn_merge_helper : NULL;
	job->join = join_merge_helper;
	// HDR: camera B's frames are hdrRatio times shorter at the same gain. hdrSensitivityB stays 0, merge.c's
	// default: a dim scene would measure it too low.
	job->hdr = request->hdr && request->frameCountB > 0;
	job->hdrRatio = 1;
	if (job->hdr) {
		float timeA = request->linesA + request->fineLines;
		float timeB = request->linesB + request->fineLines;
		job->hdrRatio = timeA / timeB;
	}
	job->gainB = request->gain;
	g_mergeSlot.succeeded = merge_run(job, &g_mergeSlot.result);
	// merge_run unpacks every frame in place before anything can fail.
	request->packed = false;
	g_mergeSlot.done = true;
}

void background_merge_start(void)
{
	if (g_mergeSlot.active || !g_mergeQueue.count || !queue_pop(&g_mergeSlot.request)) {
		return;
	}
	g_mergeSlot.shotId = g_mergeSlot.request.id;
	g_mergeSlot.log[0] = 0;
	g_mergeSlot.progress = 0;
	g_mergeSlot.succeeded = false;
	g_mergeSlot.done = false;
	g_mergeSlot.startedMs = osGetTime();
	s32 mainPriority = MAIN_THREAD_PRIORITY;
	svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);
	// Set before the thread starts: on core 2 it may run at once.
	g_mergeSlot.core = THREAD_CORE_NEW3DS;
	g_mergeSlot.threadCount = MERGE_THREADS_WANTED;
	g_mergeSlot.helperCore = HELPER_CORE_NEW3DS;
	g_mergeSlot.helperPriority = mainPriority + HELPER_STEPS_BELOW_MAIN;
	g_mergeSlot.thread = threadCreate(merge_thread, NULL, MERGE_STACK_SIZE,
	                                  mainPriority + MERGE_STEPS_BELOW_MAIN, THREAD_CORE_NEW3DS, false);
	if (!g_mergeSlot.thread) {
		// An Old 3DS has no core 2.
		g_mergeSlot.core = THREAD_CORE_APP;
		g_mergeSlot.helperCore = HELPER_CORE_OLD3DS;
		if (g_mergeSlot.threadCount > 1 && !core1_share()) {
			g_mergeSlot.threadCount = 1;
		}
		g_mergeSlot.thread =
		        threadCreate(merge_thread, NULL, MERGE_STACK_SIZE,
		                     mainPriority + MERGE_STEPS_BELOW_MAIN_OLD3DS, THREAD_CORE_APP, false);
	}
	if (!g_mergeSlot.thread) {
		// The merge runs on the main thread, without a helper.
		g_mergeSlot.threadCount = 1;
	}
	g_mergeSlot.active = true;
	if (!g_mergeSlot.thread) {
		// The UI stops for seconds, but the photo is kept.
		merge_thread(NULL);
	}
}

// A failed merge keeps frame A_00, unpacked to u16 by then, scaled to a merge's white and black.
static u16 *take_first_frame(MergeRequest *request)
{
	if (!request->framesA[0] || request->packed) {
		return NULL;
	}
	u16 *pixels = (u16 *)(void *)request->framesA[0];
	request->framesA[0] = NULL;
	for (int i = 0; i < MERGE_WIDTH * MERGE_HEIGHT; i++) {
		pixels[i] = (u16)(pixels[i] * MERGE_SCALE);
	}
	return pixels;
}

static const char *shot_name(const char *basePath)
{
	const char *slash = strrchr(basePath, '/');
	return slash ? slash + 1 : basePath;
}

static const char *review_label(bool havePhoto, bool merged)
{
	if (!havePhoto) {
		return "MERGE FAILED";
	}
	if (merged) {
		return "SOURCE FRAME (MERGED)";
	}
	return "SOURCE FRAME (MERGE FAILED)";
}

// A finished merge goes to the saver: its mosaic, or frame A_00 alone when it failed. An HDR merge's JPG is
// rendered for the metered exposure with the range above it rolled off, and the DNG says the same as
// BaselineExposure. An HDR shot that had to drop camera B is saved as a plain merge of camera A.
static void merge_finish(u64 elapsedMs)
{
	MergeRequest *request = &g_mergeSlot.request;
	MergeResult *result = &g_mergeSlot.result;
	u16 *mosaic = g_mergeSlot.succeeded ? result->mosaic : NULL;
	result->mosaic = NULL;
	bool merged = mosaic != NULL;
	if (!merged) {
		mosaic = take_first_frame(request);
	}
	const char *name = shot_name(request->basePath);
	// The review shows the shot's frame tagged "MERGING" until now.
	review_retag(name, review_label(mosaic != NULL, merged));
	if (!mosaic) {
		toast_show("Merge failed: nothing saved");
		background_merge_free_request(request);
		return;
	}
	MergeOutcome outcome = merge_report_outcome(request, result, merged);
	SaveJob job = {.kind = SAVE_MERGED,
	               .frameA = (u8 *)mosaic,
	               .format = CAM_FORMAT_RAW,
	               .width = MERGE_WIDTH,
	               .height = MERGE_HEIGHT,
	               .camSettings = request->camSettings,
	               .exposureBias = request->exposureBias,
	               .sizeDivisor = request->sizeDivisor,
	               .exposureSeconds = outcome.exposureSeconds,
	               .iso = request->iso,
	               .photoFormat = request->photoFormat,
	               .lookIndex = request->lookIndex,
	               .camera = request->cameraA,
	               .white = outcome.white,
	               .range = outcome.range,
	               .when = request->when};
	memcpy(job.whiteBalance, request->whiteBalance, sizeof job.whiteBalance);
	memcpy(job.blackLevels, request->blackLevelsA, sizeof job.blackLevels);
	job.note = merge_report_summary(request, result, &outcome);
	MergeRun run = {.mergeLog = g_mergeSlot.log,
	                .core = g_mergeSlot.core,
	                .helperCore = g_mergeSlot.helperCore,
	                .elapsedMs = elapsedMs};
	job.mergeLog = merge_report_log(request, result, &outcome, &run);
	int pathRoom = (int)(sizeof job.path - sizeof ".DNG");
	snprintf(job.path, sizeof job.path, "%.*s.DNG", pathRoom, request->basePath);
	saver_queue(&job);
	gallery_refresh_when_saved(request->id);
	merge_report_toast(name, request, result, &outcome);
	background_merge_free_request(request);
}

// Not while the save queue is full: saver_queue would wait for room here, and the viewfinder, the input and
// the APT events with it. The merge stays done in its slot until the next loop finds room.
void background_merge_poll(void)
{
	if (!g_mergeSlot.active || !g_mergeSlot.done) {
		return;
	}
	if (saver_full()) {
		return;
	}
	if (g_mergeSlot.thread) {
		threadJoin(g_mergeSlot.thread, U64_MAX);
		threadFree(g_mergeSlot.thread);
	}
	g_mergeSlot.thread = NULL;
	g_mergeSlot.active = false;
	u64 elapsedMs = osGetTime() - g_mergeSlot.startedMs;
	merge_finish(elapsedMs);
}

bool background_merge_active(void)
{
	return g_mergeSlot.active;
}

bool background_merge_is_hdr(void)
{
	return g_mergeSlot.request.hdr;
}

// The IMG number of the shot being merged.
int background_merge_shot_id(void)
{
	return g_mergeSlot.shotId;
}

// Percent done of the merge running.
int background_merge_progress(void)
{
	return g_mergeSlot.progress;
}

// How long the merge running has taken so far, 0 when none runs.
u64 background_merge_elapsed_ms(void)
{
	return g_mergeSlot.active ? osGetTime() - g_mergeSlot.startedMs : 0;
}

// Shots queued behind the merge running.
int background_merge_waiting(void)
{
	return g_mergeQueue.count;
}

// Shots whose photo is not saved yet: the one merging and the ones waiting.
int background_merge_shots_left(void)
{
	return (g_mergeSlot.active ? 1 : 0) + g_mergeQueue.count;
}
