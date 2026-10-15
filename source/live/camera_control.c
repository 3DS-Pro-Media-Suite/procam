#include "camera_control.h"
#include <3ds.h>
#include <stdatomic.h>
#include <string.h>
#include "app_threads.h"
#include "burst_shot.h"
#include "cam_settings.h"
#include "cam_stream.h"
#include "dual_stream.h"
#include "fast_view.h"
#include "preview.h"
#include "raw_access.h"
#include "saver.h"
#include "sensor_exposure.h"
#include "sensor_i2c.h"
#include "settings.h"
#include "shot_mode.h"
#include "shot_names.h"
#include "still_capture.h"
#include "still_shot.h"
#include "time_units.h"
#include "toast.h"
#include "white_balance.h"

// Reconfiguring the camera takes seconds, so the main thread posts the wanted settings and the camera thread
// applies the latest of them. Anything else that drives the camera, such as the shutter, waits for it first.

#define CAMERA_THREAD_STACK_SIZE (16 * 1024)
#define CAMERA_WAIT_POLL_NS (5 * NANOSECONDS_PER_MILLISECOND)
// A restart whose own timing writes fail is flagged again, so resyncs are spaced out.
#define RESYNC_INTERVAL_MS 10000
// No frame for WATCHDOG_MIN_MS plus three exposures means a wedged sensor.
#define WATCHDOG_MIN_MS 4000
#define WATCHDOG_EXPOSURES 3
#define WATCHDOG_EXPOSURE_MS (WATCHDOG_EXPOSURES * MILLISECONDS_PER_SECOND)

typedef struct {
	Thread thread;
	// Guards the requests and the press settings. While the main loop holds it, no camera change starts.
	LightLock lock;
	LightEvent wake;
	CamSettings wantedSettings;
	bool wantedDual; // the dual raw stream
	bool wantedFast; // the 60 fps live view of one camera
	bool wantedDualRetry; // dual started afresh, though the stream gave up on it
	_Atomic bool settingsPending;
	_Atomic bool busy;
	_Atomic bool quit;
	_Atomic bool shootPending;
	_Atomic bool resetPending; // the stream died: reset the driver and start again
	_Atomic bool guardPending; // a streaming sensor's own AE or AWB came back on: switch it off again
	_Atomic bool guarding;
	_Atomic bool away; // the app is in the background or the console asleep
	_Atomic bool stoppedForAway;
	_Atomic u64 shootingSince; // non-zero while a photo is being captured
	_Atomic int shootingMode;
	_Atomic Result lastFailure;
	ShotSettings pressSettings; // the settings at the shutter press
} CamControl;

static CamControl g_camControl;

static bool g_cameraOk;
static Result g_cameraError;

// The dual wish of the last stream start. Camera thread, or the main thread while that one idles.
static bool g_dualStarted;
// The stream wish last posted by the main thread.
static bool g_postedDual;
static bool g_postedFast;
static bool g_aptSuspended;
static bool g_aptAsleep;
// The system keyboard suspends the app too, but the stream keeps running behind it.
static bool g_inApplet;
// Take a photo on the next loop, once the UI has drawn a frame.
static bool g_fireNext;

static void fire(const ShotSettings *press);

// One camera streaming: its fast live view follows the wish.
static void switch_fast_view(bool fast)
{
	if (!cam_stream_raw_active() || dual_stream_active()) {
		return;
	}
	if (fast != fast_view_active()) {
		fast_view_switch(fast);
	}
}

// A start that wanted the dual stream got one camera. Said once per start; the mode badge keeps showing it.
static void report_dual_start(bool dualWanted)
{
	if (dualWanted && cam_stream_raw_active() && !dual_stream_active()) {
		toast_show("Dual stream did not start: one camera (change the mode to retry)");
	}
}

// Applies the settings with the stream wish. retryDual starts dual afresh when the stream gave up on it.
static Result configure_camera(const CamSettings *settings, bool dual, bool fast, bool retryDual)
{
	fast_view_set_wanted(fast);
	Result result;
	// Dual may have come up since the retry request.
	bool restartDual = retryDual && dual && g_dualStarted && cam_stream_raw_active() && !dual_stream_active();
	// cam_stream_apply keeps the stream it has, so switching dual on or off takes a start.
	if (dual != g_dualStarted || restartDual) {
		g_dualStarted = dual;
		if (restartDual) {
			dual_stream_set_wanted(false); // this clears the stream's "given up" flag
		}
		dual_stream_set_wanted(dual);
		result = cam_stream_start(settings);
		report_dual_start(dual);
	} else {
		result = cam_stream_apply(settings);
	}
	switch_fast_view(fast);
	return result;
}

// The app went away or came back: the stream stops, or starts again. Called with the lock held; releases it.
static void follow_away_change(void)
{
	bool away = g_camControl.away;
	if (!away) {
		g_camControl.resetPending = false; // the restart below is a driver reset itself
	}
	g_camControl.busy = true;
	LightLock_Unlock(&g_camControl.lock);
	preview_drain();
	if (away) {
		cam_stream_stop();
	} else {
		// Sleep may have reset the sensors and the raw set-up with them, so restart as after a stall.
		Result result = cam_stream_reset_driver();
		if (R_FAILED(result)) {
			g_camControl.lastFailure = result;
		}
	}
	g_camControl.stoppedForAway = away;
	g_camControl.busy = false;
}

// Nothing else works on a wedged sensor. Called with the lock held; releases it.
static void reset_wedged_camera(void)
{
	g_camControl.resetPending = false;
	g_camControl.busy = true;
	LightLock_Unlock(&g_camControl.lock);
	preview_drain();
	cam_stream_reset_driver();
	g_camControl.busy = false;
}

// Runs the next posted request: new settings first, so a photo uses the ones chosen last, then the shot, then
// the guard. Called with the lock held; releases it. Returns false when no request waits.
static bool run_posted_request(void)
{
	bool haveSettings = g_camControl.settingsPending;
	bool shoot = !haveSettings && g_camControl.shootPending;
	bool guard = !haveSettings && !shoot && g_camControl.guardPending;
	CamSettings settings = g_camControl.wantedSettings;
	bool dual = g_camControl.wantedDual;
	bool fast = g_camControl.wantedFast;
	bool retryDual = haveSettings && g_camControl.wantedDualRetry;
	ShotSettings press = g_camControl.pressSettings;
	g_camControl.settingsPending = false;
	if (haveSettings) {
		g_camControl.wantedDualRetry = false;
	}
	if (shoot) {
		g_camControl.shootPending = false;
		g_camControl.shootingMode = shot_mode_for_press(&press.settings);
		// Set under the lock, so camera_control_shoot sees one shot at a time.
		g_camControl.shootingSince = osGetTime();
	}
	if (guard) {
		g_camControl.guardPending = false;
		g_camControl.guarding = true;
	}
	g_camControl.busy = haveSettings || shoot || guard;
	LightLock_Unlock(&g_camControl.lock);
	if (shoot) {
		preview_drain();
		fire(&press);
		g_camControl.shootingSince = 0;
		g_camControl.busy = false;
		return true;
	}
	if (guard) {
		// The guard sleeps between its register writes, too long for the UI thread.
		sensor_exposure_guard();
		g_camControl.guarding = false;
		g_camControl.busy = false;
		return true;
	}
	if (!haveSettings) {
		return false;
	}
	preview_drain(); // a restart resets the camera buffers: the worker must not be reading one
	Result result = configure_camera(&settings, dual, fast, retryDual);
	if (R_FAILED(result)) {
		g_camControl.lastFailure = result;
	}
	g_camControl.busy = false;
	return true;
}

// Runs jobs, the most urgent first, until none is left.
static void run_jobs(void)
{
	for (;;) {
		LightLock_Lock(&g_camControl.lock);
		if (g_camControl.away != g_camControl.stoppedForAway) {
			follow_away_change();
			continue;
		}
		// Away: the requests wait for the return.
		if (g_camControl.stoppedForAway) {
			LightLock_Unlock(&g_camControl.lock);
			return;
		}
		if (g_camControl.resetPending) {
			reset_wedged_camera();
			continue;
		}
		if (!run_posted_request()) {
			return;
		}
	}
}

static void camera_thread(void *arg)
{
	(void)arg;
	for (;;) {
		LightEvent_Wait(&g_camControl.wake);
		run_jobs();
		if (g_camControl.quit) {
			break;
		}
	}
}

void camera_control_init(void)
{
	LightLock_Init(&g_camControl.lock);
	LightEvent_Init(&g_camControl.wake, RESET_ONESHOT);
	s32 mainPriority = MAIN_THREAD_PRIORITY;
	svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);
	// One above the main thread (lower is higher): its calls mostly wait on the camera, and it should start
	// at once.
	g_camControl.thread = threadCreate(camera_thread, NULL, CAMERA_THREAD_STACK_SIZE, mainPriority - 1,
	                                   THREAD_CORE_APP, false);
}

// The reset counts too: the main loop's sensor reads must not overlap it.
bool camera_control_changing(void)
{
	return g_camControl.settingsPending || g_camControl.busy || g_camControl.shootPending ||
	       g_camControl.guardPending || g_camControl.resetPending || g_camControl.away ||
	       g_camControl.stoppedForAway;
}

// Not the guard: it is short and runs often in auto.
bool camera_control_applying(void)
{
	return g_camControl.settingsPending || g_camControl.shootPending || g_camControl.resetPending ||
	       g_camControl.away || g_camControl.stoppedForAway || (g_camControl.busy && !g_camControl.guarding);
}

// The capture runs on the camera thread, since bursts take seconds. The settings of the press go with it.
static void camera_control_shoot(void)
{
	if (!g_cameraOk) {
		return;
	}
	LightLock_Lock(&g_camControl.lock);
	bool shotRunning = g_camControl.thread && (g_camControl.shootPending || g_camControl.shootingSince);
	if (!shotRunning) {
		g_camControl.pressSettings.settings = g_settings;
		white_balance_get(g_camControl.pressSettings.whiteBalance);
		g_camControl.pressSettings.hotPixelFix = g_hotPixelFix;
		g_camControl.shootPending = g_camControl.thread != NULL;
	}
	LightLock_Unlock(&g_camControl.lock);
	if (shotRunning) {
		return;
	}
	if (!g_camControl.thread) {
		fire(&g_camControl.pressSettings);
		return;
	}
	LightEvent_Signal(&g_camControl.wake);
}

// Waits until the camera has the settings last asked for. Not while the app is away: they wait for it.
static void wait_for_camera(void)
{
	while (g_camControl.thread && camera_control_changing() && !g_camControl.away) {
		svcSleepThread(CAMERA_WAIT_POLL_NS);
	}
}

static void post_settings(const CamSettings *settings, bool retryDual)
{
	if (!g_cameraOk) {
		return;
	}
	g_postedDual = shot_mode_dual_wanted(&g_settings);
	g_postedFast = shot_mode_fast_wanted(&g_settings);
	if (!g_camControl.thread) {
		Result result = configure_camera(settings, g_postedDual, g_postedFast, retryDual);
		if (R_FAILED(result)) {
			g_camControl.lastFailure = result;
		}
		return;
	}
	LightLock_Lock(&g_camControl.lock);
	g_camControl.wantedSettings = *settings;
	g_camControl.wantedDual = g_postedDual;
	g_camControl.wantedFast = g_postedFast;
	// A later request does not take back a retry that has not run yet.
	g_camControl.wantedDualRetry |= retryDual;
	g_camControl.settingsPending = true;
	LightLock_Unlock(&g_camControl.lock);
	LightEvent_Signal(&g_camControl.wake);
}

void camera_control_request(const CamSettings *settings)
{
	post_settings(settings, false);
}

// The camera only hears of the change when the stream it should run changes. MERGE, HDR and 3D all want the
// same stream, so choosing one of them anew is the retry for a dual stream the camera gave up on.
void camera_control_request_stream(bool modeChanged)
{
	bool retryDual = modeChanged && shot_mode_dual_wanted(&g_settings) && g_postedDual &&
	                 cam_stream_raw_active() && !dual_stream_active();
	if (retryDual || shot_mode_dual_wanted(&g_settings) != g_postedDual ||
	    shot_mode_fast_wanted(&g_settings) != g_postedFast) {
		post_settings(&g_settings.cam, retryDual);
	}
}

// Starts the stream for the live settings on the calling thread.
static Result start_stream_now(void)
{
	g_dualStarted = shot_mode_dual_wanted(&g_settings);
	g_postedDual = g_dualStarted;
	g_postedFast = shot_mode_fast_wanted(&g_settings);
	dual_stream_set_wanted(g_dualStarted);
	fast_view_set_wanted(g_postedFast);
	Result result = cam_stream_start(&g_settings.cam);
	report_dual_start(g_dualStarted);
	return result;
}

// Reaches the sensors directly through a copy of the camera module's i2c session.
static void use_raw_sensors(void)
{
	Handle i2cSession = 0;
	Result result = raw_access_steal_i2c_session(&i2cSession);
	if (R_FAILED(result)) {
		return;
	}
	sensor_i2c_set_session(i2cSession);
	cam_stream_set_raw_mode(true);
	g_rawMode = true;
}

void camera_control_start(bool onCitra, bool displayReady)
{
	g_cameraError = cam_stream_init();
	g_cameraOk = R_SUCCEEDED(g_cameraError) && displayReady;
	if (!g_cameraOk) {
		return;
	}
	if (!onCitra) {
		use_raw_sensors();
	}
	// Starts both outer cameras at once when the saved mode is MERGE, HDR or 3D.
	Result result = start_stream_now();
	if (R_FAILED(result)) {
		CamStreamInfo streamInfo;
		cam_stream_info(&streamInfo);
		const char *failedCall = streamInfo.lastErrorCall ? streamInfo.lastErrorCall : "?";
		// Only the function's name: the recorded call text has its arguments too.
		int callNameLength = (int)strcspn(failedCall, "(");
		toast_show("Start: 0x%08lX in %.*s", result, callNameLength, failedCall);
	}
}

bool camera_control_ok(void)
{
	return g_cameraOk;
}

Result camera_control_error(void)
{
	return g_cameraError;
}

// The guard sleeps between its register writes, so it runs on the camera thread.
static void post_guard(void)
{
	if (!g_camControl.thread) {
		sensor_exposure_guard();
		return;
	}
	LightLock_Lock(&g_camControl.lock);
	g_camControl.guardPending = true;
	LightLock_Unlock(&g_camControl.lock);
	LightEvent_Signal(&g_camControl.wake);
}

// Called after the main loop's own sensor accesses, which must not overlap the camera thread's work.
void camera_control_poll_stream(u64 nowMs)
{
	if (dual_stream_broken()) {
		// One camera until a mode is chosen anew. cam_stream_apply then finds dual impossible and restarts,
		// so this toast comes once.
		toast_show("Dual stream unstable: one camera (change the mode to retry)");
		camera_control_request(&g_settings.cam);
	} else if (sensor_exposure_guard_wanted()) {
		post_guard();
	} else if (dual_stream_active()) {
		// A burst found the two sensors' frames apart. A restart now, while nothing is being shot,
		// re-synchronises them; otherwise every 3D pair takes the slow way.
		static u64 lastResyncAt;
		if (dual_stream_desync() && nowMs - lastResyncAt > RESYNC_INTERVAL_MS) {
			lastResyncAt = nowMs;
			camera_control_request(&g_settings.cam);
		}
	}
}

void camera_control_exit(void)
{
	if (!g_camControl.thread) {
		return;
	}
	wait_for_camera();
	g_camControl.quit = true;
	LightEvent_Signal(&g_camControl.wake);
	threadJoin(g_camControl.thread, U64_MAX);
	threadFree(g_camControl.thread);
	g_camControl.thread = NULL;
}

// Only the live view's port counts: the dual stream's other camera streams only for a burst.
void camera_control_watchdog(void)
{
	static u32 lastFrameCount;
	static u64 lastFrameAt;
	u64 nowMs = osGetTime();
	u32 frameCount = cam_stream_frame_count();
	// lastFrameAt 0 is the first pass: osGetTime counts from 1900, so the watchdog would fire at each launch.
	if (!lastFrameAt || frameCount != lastFrameCount || camera_control_changing() || !g_cameraOk ||
	    !g_camControl.thread) {
		lastFrameCount = frameCount;
		lastFrameAt = nowMs;
	}
	u64 stallLimitMs = WATCHDOG_MIN_MS;
	if (cam_stream_raw_active() && g_settings.cam.exposureMode != CAM_EXPOSURE_AUTO) {
		float shutterSeconds = CAM_SHUTTER_SECONDS[(int)g_settings.cam.shutter];
		stallLimitMs += (u64)(shutterSeconds * WATCHDOG_EXPOSURE_MS);
	}
	// A port that restarts over and over still lets frames trickle in, so the time limit alone never fires.
	bool restartStorm = cam_stream_restart_storm();
	bool stalled = nowMs - lastFrameAt > stallLimitMs;
	if (stalled || (restartStorm && !camera_control_changing())) {
		lastFrameAt = nowMs;
		toast_show("Camera stalled: restarting it");
		LightLock_Lock(&g_camControl.lock);
		g_camControl.resetPending = true;
		LightLock_Unlock(&g_camControl.lock);
		LightEvent_Signal(&g_camControl.wake);
	}
}

void camera_control_apt_hook(APT_HookType type, void *param)
{
	(void)param;
	if (g_inApplet && (type == APTHOOK_ONSUSPEND || type == APTHOOK_ONRESTORE)) {
		return;
	}
	if (type == APTHOOK_ONSUSPEND) {
		g_aptSuspended = true;
	} else if (type == APTHOOK_ONRESTORE) {
		g_aptSuspended = false;
	} else if (type == APTHOOK_ONSLEEP) {
		g_aptAsleep = true;
	} else if (type == APTHOOK_ONWAKEUP) {
		g_aptAsleep = false;
	} else {
		return;
	}
	if (!g_cameraOk || !g_camControl.thread) {
		return;
	}
	LightLock_Lock(&g_camControl.lock);
	g_camControl.away = g_aptSuspended || g_aptAsleep;
	if (type == APTHOOK_ONWAKEUP) {
		g_camControl.resetPending = true; // a sleep that ends while the thread is busy still restarts
	}
	LightLock_Unlock(&g_camControl.lock);
	LightEvent_Signal(&g_camControl.wake);
}

void camera_control_set_in_applet(bool open)
{
	g_inApplet = open;
}

// The shot, on the camera thread, with the settings of the press and nothing live. Without raw access (the
// YUV stream) PHOTO, BRACKET and 3D come from the camera service's stills, and MERGE and HDR take a photo.
static void fire(const ShotSettings *press)
{
	const Settings *settings = &press->settings;
	int mode = shot_mode_for_press(settings);
	bool rawBefore = cam_stream_raw_active();
	bool dualBefore = dual_stream_active();
	preview_drain(); // captures restart the stream: the preview worker must not be reading a camera buffer
	bool photoInstead = mode != shot_mode_get(settings);
	g_camControl.shootingMode = mode;
	// MERGE, HDR and raw 3D play their sound when the frames are in, so it tells when the camera may move.
	bool lateSound = rawBefore && (mode == MODE_MERGE || mode == MODE_HDR || mode == MODE_3D);
	still_capture_set_late_sound(settings->shutterSound && lateSound);
	if (settings->shutterSound && !lateSound) {
		CAMU_PlayShutterSound(SHUTTER_SOUND_TYPE_NORMAL);
	}
	if (mode == MODE_MERGE || mode == MODE_HDR) {
		burst_shot_merge(press, mode == MODE_HDR);
	} else if (mode == MODE_3D) {
		burst_shot_3d(press);
	} else {
		still_shot_take(press, mode == MODE_BRACKET);
	}
	// A capture that never came leaves no sound due for the next one.
	still_capture_set_late_sound(false);
	if (photoInstead) {
		toast_show("%s needs the raw stream: took a photo (%s)", MODE_SHORT_NAMES[shot_mode_get(settings)],
		           g_lastShot);
	} else if (dualBefore && rawBefore && cam_stream_raw_active() && !dual_stream_active()) {
		// The camera gave the dual stream up during the shot. Its fallback restarted one camera, which the
		// main loop's broken stream check does not see.
		toast_show("Dual stream given up: one camera (change the mode to retry)");
	}
}

// Not while the camera thread is starting the stream.
bool camera_control_dual_lost(const Settings *settings)
{
	return shot_mode_dual_wanted(settings) && g_cameraOk && cam_stream_raw_active() &&
	       !camera_control_changing() && !dual_stream_active();
}

void camera_control_request_shot(void)
{
	g_fireNext = true;
}

void camera_control_fire_pending(void)
{
	if (g_fireNext) {
		g_fireNext = false;
		camera_control_shoot();
	}
}

void camera_control_lock(void)
{
	LightLock_Lock(&g_camControl.lock);
}

void camera_control_unlock(void)
{
	LightLock_Unlock(&g_camControl.lock);
}

u64 camera_control_shooting_since(void)
{
	return g_camControl.shootingSince;
}

int camera_control_shooting_mode(void)
{
	return g_camControl.shootingMode;
}

Result camera_control_take_failure(void)
{
	return atomic_exchange(&g_camControl.lastFailure, 0);
}
