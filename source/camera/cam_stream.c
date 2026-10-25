#include "cam_stream.h"
#include <3ds.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "cam_config.h"
#include "cam_settings.h"
#include "cam_state.h"
#include "capture_thread.h"
#include "dual_stream.h"
#include "dual_stream_internal.h"
#include "fast_view.h"
#include "sensor_exposure.h"
#include "sensor_i2c.h"
#include "thread_defaults.h"
#include "time_units.h"

#define CAPTURE_THREAD_STACK_SIZE 0x4000
#define CAPTURE_THREAD_JOIN_TIMEOUT_NS (2 * NANOSECONDS_PER_SECOND)

// Thread priorities: a lower value runs first. The capture thread runs CAPTURE_PRIORITY_LEAD above the main
// thread, or one step above it near CAPTURE_PRIORITY_HIGHEST.
#define CAPTURE_PRIORITY_LEAD 8
#define CAPTURE_PRIORITY_HIGHEST 0x19

// A port without a frame for this long is restarted.
#define DEFAULT_STALL_MS 1500

// The capture thread mostly sleeps, and a re-arm must never wait behind the main thread or the app's camera
// control thread (main - 1).
static s32 capture_thread_priority(s32 mainPriority)
{
	if (mainPriority - CAPTURE_PRIORITY_LEAD >= CAPTURE_PRIORITY_HIGHEST) {
		return mainPriority - CAPTURE_PRIORITY_LEAD;
	}
	if (mainPriority - 1 >= CAPTURE_PRIORITY_HIGHEST) {
		return mainPriority - 1;
	}
	return mainPriority;
}

// The live view's ring on the single stream's port. All or nothing.
static bool allocate_view_ring(void)
{
	CamPort *port = &g_camera.ports[CAM_PORT_A];
	bool allocated = true;
	for (int i = 0; i < CAM_RING_FRAMES; i++) {
		port->ring[i] = malloc(CAM_FRAME_BYTES);
		if (!port->ring[i]) {
			allocated = false;
		}
	}
	if (allocated) {
		return true;
	}
	for (int i = 0; i < CAM_RING_FRAMES; i++) {
		free(port->ring[i]);
		port->ring[i] = NULL;
	}
	return false;
}

static void close_error_event(CamPort *port)
{
	if (port->errorEvent) {
		svcCloseHandle(port->errorEvent);
		port->errorEvent = 0;
	}
}

Result cam_stream_init(void)
{
	if (g_camera.initialized) {
		return 0;
	}
	Result result = camInit();
	if (R_FAILED(result)) {
		return result;
	}
	// The camera module keeps its configuration between sessions, even after a crash: start from a clean
	// driver.
	CAMU_DriverFinalize();
	CAMU_DriverInitialize();
	if (!allocate_view_ring()) {
		camExit();
		return CAM_ERROR_NO_MEMORY;
	}
	LightLock_Init(&g_camera.lock);
	LightEvent_Init(&g_camera.burst.done, RESET_ONESHOT);
	for (int c = 0; c < CAM_PORT_COUNT; c++) {
		g_camera.ports[c].ready = -1;
		g_camera.ports[c].reading = -1;
		g_camera.ports[c].armedSlot = -1;
	}
	g_camera.streamingPorts = CAM_PORT_MASK_A;
	g_camera.portIds[CAM_PORT_A] = PORT_CAM1;
	g_camera.portIds[CAM_PORT_B] = PORT_CAM2;
	g_camera.stallMs = DEFAULT_STALL_MS;
	if (R_FAILED(svcCreateEvent(&g_camera.wakeEvent, RESET_ONESHOT))) {
		g_camera.wakeEvent = 0; // without the event the capture thread's waits time out instead
	}
	g_camera.haveCalibration = R_SUCCEEDED(CAMU_GetStereoCameraCalibrationData(&g_camera.calibration));
	s32 mainPriority = MAIN_THREAD_PRIORITY;
	svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);
	g_camera.thread = threadCreate(capture_thread_run, NULL, CAPTURE_THREAD_STACK_SIZE,
	                               capture_thread_priority(mainPriority), THREAD_CORE_APP, false);
	g_camera.initialized = true;
	return 0;
}

// Returns whether the thread has ended. One still inside a slow camera service call keeps its wake event and
// finishes that call first: quit stays set, so it ends right after.
static bool end_capture_thread(void)
{
	g_camera.quit = true;
	if (g_camera.wakeEvent) {
		svcSignalEvent(g_camera.wakeEvent);
	}
	// A timeout returns a code that R_SUCCEEDED accepts, so only 0 means the thread has ended.
	if (g_camera.thread && threadJoin(g_camera.thread, CAPTURE_THREAD_JOIN_TIMEOUT_NS) != 0) {
		return false;
	}
	threadFree(g_camera.thread);
	g_camera.thread = NULL;
	if (g_camera.wakeEvent) {
		svcCloseHandle(g_camera.wakeEvent);
		g_camera.wakeEvent = 0;
	}
	return true;
}

void cam_stream_exit(void)
{
	if (!g_camera.initialized) {
		return;
	}
	sensor_exposure_park();
	capture_thread_pause();
	bool threadEnded = end_capture_thread();
	// Queued behind any call of the capture thread: no receive it armed writes into the buffers freed below.
	CAMU_StopCapture(PORT_BOTH);
	if (g_camera.dual) {
		dual_stream_exit();
	}
	for (int c = 0; c < CAM_PORT_COUNT; c++) {
		close_error_event(&g_camera.ports[c]);
	}
	CAMU_Activate(SELECT_NONE);
	sensor_i2c_close_session();
	// A driver reset gives the next application the module's defaults instead of the sensors' raw and fast
	// modes with the module's AE / AWB off.
	CAMU_DriverFinalize();
	CAMU_DriverInitialize();
	camExit();
	for (int c = 0; c < CAM_PORT_COUNT; c++) {
		for (int i = 0; i < CAM_RING_FRAMES; i++) {
			free(g_camera.ports[c].ring[i]);
		}
	}
	free(g_camera.sink);
	// A thread that has not ended still reads g_camera, its quit flag and lock included.
	if (threadEnded) {
		memset(&g_camera, 0, sizeof g_camera);
	}
}

static bool wants_dual_stream(const CamSettings *settings)
{
	return g_camera.dualWanted && !g_camera.dualBroken && g_camera.rawMode && sensor_i2c_has_session() &&
	       settings->camera != CAM_INNER;
}

// Stop the ports and the capture thread before the stream changes.
static void stop_capture(void)
{
	sensor_exposure_park();
	capture_thread_pause();
	CAMU_StopCapture(PORT_BOTH);
}

// A size other than `size`. The module only reprograms the sensor when the size changes, so asking for
// another size first forces it to take the format along.
static CAMU_Size other_size(CAMU_Size size)
{
	return size == SIZE_QQVGA ? SIZE_QVGA : SIZE_QQVGA;
}

static void select_camera(const CamSettings *settings)
{
	g_camera.cameraSelect = CAM_SERVICE_SELECT[(int)settings->camera];
	g_camera.livePort = CAM_SERVICE_PORT[(int)settings->camera];
	g_camera.portIds[CAM_PORT_A] = g_camera.livePort;
	g_camera.frameWidth = CAM_FRAME_WIDTH;
	g_camera.frameHeight = CAM_FRAME_HEIGHT;
}

// Every camera service call on an active camera waits about two sensor frames, which can add up to seconds
// in the dark. So the camera is switched off, configured (the module only stores the settings) and switched
// on once.
static void configure_inactive_camera(const CamSettings *settings, CAMU_OutputFormat outputFormat,
                                      Result *firstError)
{
	Result err = *firstError;
	CAM_TRY(CAMU_Activate(SELECT_NONE));
	CAM_TRY_AS("CAMU_SetSize(S.sel, s.size == SIZE_QQVGA ? SIZE_QVGA : SIZE_QQVGA, CONTEXT_A)",
	           CAMU_SetSize(g_camera.cameraSelect, other_size(settings->size), CONTEXT_A));
	CAM_TRY_AS("CAMU_SetSize(S.sel, s.size == SIZE_QQVGA ? SIZE_QVGA : SIZE_QQVGA, CONTEXT_B)",
	           CAMU_SetSize(g_camera.cameraSelect, other_size(settings->size), CONTEXT_B));
	CAM_TRY_AS("CAMU_SetSize(S.sel, s.size, CONTEXT_A)",
	           CAMU_SetSize(g_camera.cameraSelect, settings->size, CONTEXT_A));
	CAM_TRY_AS("CAMU_SetOutputFormat(S.sel, fmt, CONTEXT_A)",
	           CAMU_SetOutputFormat(g_camera.cameraSelect, outputFormat, CONTEXT_A));
	// Context B is the still photo context: always the full frame.
	CAM_TRY_AS("CAMU_SetSize(S.sel, SIZE_VGA, CONTEXT_B)",
	           CAMU_SetSize(g_camera.cameraSelect, SIZE_VGA, CONTEXT_B));
	CAM_TRY_AS("CAMU_SetOutputFormat(S.sel, fmt, CONTEXT_B)",
	           CAMU_SetOutputFormat(g_camera.cameraSelect, outputFormat, CONTEXT_B));
	// The preview runs in context A. A session that ended during a still capture leaves the module in B.
	CAM_TRY_AS("CAMU_SwitchContext(S.sel, CONTEXT_A)", CAMU_SwitchContext(g_camera.cameraSelect, CONTEXT_A));
	*firstError = err;
}

// The image controls the module gets. In raw mode the app sets the exposure itself, so the module runs a
// fixed 30 fps with its own AE and AWB off: a variable rate let its AE stretch the frame in the dark, and
// nothing in raw mode shortens it again.
static CamSettings module_image_controls(const CamSettings *settings)
{
	CamSettings controls = *settings;
	if (g_camera.rawMode) {
		controls.frameRate = FRAME_RATE_30;
		controls.autoExposure = 0;
		controls.awbLock = 1;
	}
	return controls;
}

// Every image control, not only the changed ones: the module keeps its state between sessions, so an
// earlier session can leave anything behind.
static void apply_all_image_controls(const CamSettings *controls, Result *firstError)
{
	Result err = *firstError;
	CamSettings unset;
	memset(&unset, CAM_SETTINGS_UNSET_BYTE, sizeof unset);
	Result controlsErr = cam_config_apply_image_controls(g_camera.cameraSelect, controls, &unset);
	if (R_SUCCEEDED(err)) {
		err = controlsErr;
	}
	if (g_camera.rawMode) {
		// Raw keeps the sensor's own pixel order; the viewer mirrors the image. Flipping is only fast while
		// the camera is off.
		CAM_TRY_AS("CAMU_FlipImage(S.sel, FLIP_NONE, CONTEXT_A)",
		           CAMU_FlipImage(g_camera.cameraSelect, FLIP_NONE, CONTEXT_A));
		CAM_TRY_AS("CAMU_FlipImage(S.sel, FLIP_NONE, CONTEXT_B)",
		           CAMU_FlipImage(g_camera.cameraSelect, FLIP_NONE, CONTEXT_B));
	}
	*firstError = err;
}

// No raw output after all: the YUV stream gets its flip, its frame rate, AE and AWB back.
static void restore_yuv_controls(const CamSettings *settings, const CamSettings *moduleControls,
                                 Result *firstError)
{
	Result err = *firstError;
	CAM_TRY_AS("CAMU_FlipImage(S.sel, (CAMU_Flip)s.flip, CONTEXT_A)",
	           CAMU_FlipImage(g_camera.cameraSelect, (CAMU_Flip)settings->flip, CONTEXT_A));
	CAM_TRY_AS("CAMU_FlipImage(S.sel, (CAMU_Flip)s.flip, CONTEXT_B)",
	           CAMU_FlipImage(g_camera.cameraSelect, (CAMU_Flip)settings->flip, CONTEXT_B));
	if (moduleControls->frameRate != settings->frameRate) {
		CAM_TRY_AS("CAMU_SetFrameRate(S.sel, s.frameRate)",
		           CAMU_SetFrameRate(g_camera.cameraSelect, settings->frameRate));
	}
	if (moduleControls->autoExposure != settings->autoExposure) {
		CAM_TRY_AS("CAMU_SetAutoExposure(S.sel, s.autoExposure)",
		           CAMU_SetAutoExposure(g_camera.cameraSelect, settings->autoExposure));
	}
	if (moduleControls->awbLock != settings->awbLock) {
		bool autoWhiteBalance = settings->whiteBalance == WHITE_BALANCE_AUTO && !settings->awbLock;
		CAM_TRY_AS("CAMU_SetAutoWhiteBalance(S.sel, s.whiteBalance == 0 && !s.awbLock)",
		           CAMU_SetAutoWhiteBalance(g_camera.cameraSelect, autoWhiteBalance));
	}
	*firstError = err;
}

// Configured while the camera is off, the preview context can keep streaming YUV when RGB565 is asked for.
// On an active camera a size change makes the module reprogram the sensor with the format. YUV is the
// power-on format and needs none of this.
static void force_rgb565_output(const CamSettings *settings, Result *firstError)
{
	Result err = *firstError;
	CAM_TRY_AS("CAMU_SetSize(S.sel, s.size == SIZE_QQVGA ? SIZE_QVGA : SIZE_QQVGA, CONTEXT_A)",
	           CAMU_SetSize(g_camera.cameraSelect, other_size(settings->size), CONTEXT_A));
	CAM_TRY_AS("CAMU_SetSize(S.sel, s.size, CONTEXT_A)",
	           CAMU_SetSize(g_camera.cameraSelect, settings->size, CONTEXT_A));
	CAM_TRY_AS("CAMU_SetOutputFormat(S.sel, fmt, CONTEXT_A)",
	           CAMU_SetOutputFormat(g_camera.cameraSelect, OUTPUT_RGB_565, CONTEXT_A));
	*firstError = err;
}

// The full frame length on the raw sensor, then the fast live view when it is wanted.
static void set_up_raw_frame(s8 camera)
{
	// The fast view works on the sensor of g_camera.settings.
	g_camera.settings.camera = camera;
	fast_view_fix_frame_length();
	if (g_camera.fastViewWanted) {
		fast_view_set_sensor(true);
	}
	fast_view_update_frame_size();
}

// Both ports' buffers and error flags: a raw burst or a dual stream before this start can leave the other
// port stopped in the middle of an overrun. The idle port's result does not count.
static void clear_port_buffers(Result *firstError)
{
	Result err = *firstError;
	CamPort *port = &g_camera.ports[CAM_PORT_A];
	CAM_TRY_AS("CAMU_ClearBuffer(S.port)", CAMU_ClearBuffer(g_camera.livePort));
	CAMU_ClearBuffer(PORT_BOTH & ~g_camera.livePort);
	close_error_event(port);
	CAM_TRY_AS("CAMU_GetBufferErrorInterruptEvent(&S.P[0].errEvt, S.port)",
	           CAMU_GetBufferErrorInterruptEvent(&port->errorEvent, g_camera.livePort));
	// The capture thread waits on this event: an overrun from before this start would restart the port.
	if (port->errorEvent) {
		svcClearEvent(port->errorEvent);
	}
	*firstError = err;
}

static Result start_capture(const CamSettings *settings, Result err)
{
	g_camera.streamingPorts = CAM_PORT_MASK_A;
	g_camera.lastError = err;
	if (R_FAILED(CAMU_StartCapture(g_camera.livePort))) {
		err = CAM_ERROR_START_CAPTURE;
		g_camera.lastError = err;
		g_camera.lastErrorCall = "CAMU_StartCapture";
		return err;
	}
	// Before sensor_exposure_apply, which works on the camera in g_camera.settings.
	g_camera.settings = *settings;
	g_camera.running = true;
	g_camera.ports[CAM_PORT_A].frames = 0;
	g_camera.ports[CAM_PORT_A].fps = 0;
	capture_thread_resume();
	// After StartCapture, with a receive already waiting so the write cannot overrun the port: set before it,
	// the sensor's own AE and AWB come back on.
	if (g_camera.rawActive) {
		sensor_exposure_apply(settings);
		sensor_exposure_ask_guard(); // a check once the start's window has passed
	}
	return err;
}

Result cam_stream_start(const CamSettings *requested)
{
	if (!g_camera.initialized) {
		return CAM_ERROR_NOT_INITIALIZED;
	}
	CamSettings settings = *requested;
	cam_settings_clamp(&settings);
	Result err = 0;
	g_camera.lastErrorCall = NULL;

	stop_capture();
	fast_view_set_sensor(false); // the sensor that streamed so far goes back to its full frame
	g_camera.running = false;
	if (wants_dual_stream(&settings)) {
		if (dual_stream_start(&settings, &err)) {
			return err;
		}
		// Dual failed: one camera, and that start reports its own result.
		err = 0;
		g_camera.lastErrorCall = NULL;
	} else {
		dual_stream_leave(); // camera B goes back to normal while it is still awake
	}
	select_camera(&settings);
	CAMU_OutputFormat outputFormat = settings.format == CAM_FORMAT_RGB565 ? OUTPUT_RGB_565 : OUTPUT_YUV_422;

	configure_inactive_camera(&settings, outputFormat, &err);
	CamSettings moduleControls = module_image_controls(&settings);
	apply_all_image_controls(&moduleControls, &err);
	CAM_TRY_AS("CAMU_Activate(S.sel)", CAMU_Activate(g_camera.cameraSelect));
	g_camera.rawActive = false;
	if (g_camera.rawMode) {
		g_camera.rawActive = R_SUCCEEDED(sensor_i2c_set_raw_output(settings.camera, true));
		if (!g_camera.rawActive) {
			restore_yuv_controls(&settings, &moduleControls, &err);
		}
	}
	if (outputFormat == OUTPUT_RGB_565 && !g_camera.rawActive) {
		force_rgb565_output(&settings, &err);
	}
	if (g_camera.rawActive) {
		set_up_raw_frame(settings.camera);
	}
	cam_config_stream_transfer(&err);
	clear_port_buffers(&err);
	return start_capture(&settings, err);
}

// While the app is away the sensors wait in standby. The steps are the exit's, but every session stays open.
void cam_stream_stop(void)
{
	if (!g_camera.initialized) {
		return;
	}
	stop_capture();
	g_camera.running = false;
	// Camera B goes back to normal while it is still awake.
	if (g_camera.dual) {
		dual_stream_exit();
	}
	CAMU_Activate(SELECT_NONE);
}

static bool needs_restart(const CamSettings *settings)
{
	const CamSettings *current = &g_camera.settings;
	bool dual = wants_dual_stream(settings);
	if (!g_camera.running || dual != g_camera.dual) {
		return true;
	}
	if (settings->camera != current->camera || settings->size != current->size ||
	    settings->format != current->format) {
		return true;
	}
	if (!dual) {
		return false;
	}
	// A time the dual frame does not fit, or a frame longer than the time needs: only a restart sets the
	// frame length of both sensors together.
	bool timeChanged =
	        settings->exposureMode != current->exposureMode || settings->shutter != current->shutter;
	if (timeChanged && dual_stream_frame_lines(settings) != g_camera.dualFrameLines) {
		return true;
	}
	// A burst found the two sensors' frames apart: a start synchronises them again.
	return g_camera.dualDesync;
}

Result cam_stream_apply(const CamSettings *requested)
{
	CamSettings settings = *requested;
	cam_settings_clamp(&settings);
	if (needs_restart(&settings)) {
		return cam_stream_start(&settings);
	}
	g_camera.lastErrorCall = NULL;
	Result err = 0;
	// Raw: the service's image controls only reach the bypassed image processor, and its SetExposure switches
	// the sensor's own AE back on. The app sets the raw exposure itself, so they are skipped.
	if (!g_camera.rawActive) {
		err = cam_config_apply_image_controls(g_camera.cameraSelect, &settings, &g_camera.settings);
	}
	bool exposureChanged = settings.exposureMode != g_camera.settings.exposureMode ||
	                       settings.shutter != g_camera.settings.shutter ||
	                       settings.iso != g_camera.settings.iso;
	if (exposureChanged) {
		Result exposureErr = sensor_exposure_apply(&settings);
		if (R_SUCCEEDED(err)) {
			err = exposureErr;
		}
	}
	g_camera.lastError = err;
	g_camera.settings = settings;
	return err;
}

// Wrap-safe: true until the port's frame count passes g_camera.hideFramesUntil.
static bool frame_is_hidden(const CamPort *port)
{
	return (s32)(port->framesDone - g_camera.hideFramesUntil) <= 0;
}

bool cam_stream_poll(u16 **pixels, int *width, int *height)
{
	if (!g_camera.running) {
		return false;
	}
	CamPort *port = cam_state_view_port();
	LightLock_Lock(&g_camera.lock);
	bool hasFrame = port->newFrame && port->ready >= 0;
	int frameIndex = port->ready;
	// An HDR burst's frames, taken at its own times, and the two after it restores the sensors are not shown.
	if (hasFrame && frame_is_hidden(port)) {
		port->newFrame = false;
		hasFrame = false;
	}
	if (hasFrame) {
		port->reading = frameIndex;
		port->newFrame = false;
	}
	LightLock_Unlock(&g_camera.lock);
	if (!hasFrame) {
		return false;
	}
	*pixels = port->ring[frameIndex];
	*width = g_camera.frameWidth;
	*height = g_camera.frameHeight;
	return true;
}

// For a sensor that stopped answering I2C, so that every camera service call fails. Raw mode is started again
// too.
Result cam_stream_reset_driver(void)
{
	if (!g_camera.initialized) {
		return CAM_ERROR_NOT_INITIALIZED;
	}
	CamSettings settings = g_camera.settings;
	capture_thread_pause();
	CAMU_StopCapture(PORT_BOTH);
	g_camera.running = false;
	// The driver reset puts the sensors back to their own state (raw off, their own frame length), so only
	// the dual stream's bookkeeping is left to undo.
	g_camera.rawActive = false;
	g_camera.fastViewOn = false;
	if (g_camera.dual) {
		dual_stream_free();
	}
	CAMU_Activate(SELECT_NONE);
	CAMU_DriverFinalize();
	Result resetResult = CAMU_DriverInitialize();
	close_error_event(&g_camera.ports[CAM_PORT_A]);
	Result startResult = cam_stream_start(&settings);
	return R_FAILED(resetResult) ? resetResult : startResult;
}

u32 cam_stream_frame_count(void)
{
	return cam_state_view_port()->frames;
}

void cam_stream_info(CamStreamInfo *info)
{
	memset(info, 0, sizeof *info);
	if (!g_camera.initialized) {
		return;
	}
	// Not CAMU_GetActivatedCamera: the camera service handles one call at a time, so it would wait behind
	// whatever slow call the camera control thread is making.
	info->activated = g_camera.running ? g_camera.cameraSelect : 0;
	info->haveCalibration = g_camera.haveCalibration;
	info->calibration = g_camera.calibration;
	info->fps = cam_state_view_port()->fps;
	info->frames = cam_state_view_port()->frames;
	info->restarts = g_camera.restarts;
	info->lastError = g_camera.lastError;
	info->lastErrorCall = g_camera.lastErrorCall;
}

float cam_stream_fps(void)
{
	return cam_state_view_port()->fps;
}

int cam_stream_format(void)
{
	return g_camera.rawActive ? CAM_FORMAT_RAW : g_camera.settings.format;
}

void cam_stream_set_raw_mode(bool enabled)
{
	g_camera.rawMode = enabled;
}

bool cam_stream_raw_active(void)
{
	return g_camera.rawActive;
}

bool cam_stream_restart_storm(void)
{
	return atomic_exchange(&g_camera.restartStorm, false);
}
