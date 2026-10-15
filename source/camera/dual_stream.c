#include "dual_stream.h"
#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include "cam_config.h"
#include "cam_settings.h"
#include "cam_state.h"
#include "capture_thread.h"
#include "dual_stream_internal.h"
#include "sensor_exposure.h"
#include "sensor_i2c.h"

#define DUAL_CAMERA_SELECT (SELECT_OUT1 | SELECT_OUT2)
#define FRAME_LINES_MAX 0xFFFF // FRAME_LENGTH_LINES is a 16-bit register

// A sensor's own frame length outside this range is not one the module programmed: the default stays. The
// range is wide because the module's AE may have stretched the frame, and that length is given back too.
#define SAVED_FRAME_LINES_MIN 400
#define SAVED_FRAME_LINES_MAX 2000

static const u8 PORT_SENSOR_DEVICES[CAM_PORT_COUNT] = {SENSOR_DEVICE_OUT1, SENSOR_DEVICE_OUT2};

// Frames never stretch in dual: a stretch lands on one sensor first and the two drift apart for good. A
// manual time that does not fit gets a longer fixed frame; other times are capped to it.
u16 dual_stream_frame_lines(const CamSettings *settings)
{
	u16 manualLines;
	u16 fineTime;
	sensor_exposure_manual_time(settings, &manualLines, &fineTime); // auto: 0 lines
	u32 frameLines = (u32)manualLines + SENSOR_EXPOSURE_MARGIN_LINES;
	if (frameLines <= DUAL_FRAME_LINES) {
		return DUAL_FRAME_LINES;
	}
	if (frameLines < FRAME_LINES_MAX) {
		return (u16)frameLines;
	}
	return FRAME_LINES_MAX;
}

// Camera B's ring exists only while camera B is the live view.
static bool set_camera_b_ring(bool wanted)
{
	CamPort *cameraB = &g_camera.ports[CAM_PORT_B];
	if (!wanted) {
		for (int i = 0; i < CAM_RING_FRAMES; i++) {
			free(cameraB->ring[i]);
			cameraB->ring[i] = NULL;
		}
		return true;
	}
	if (cameraB->ring[0]) {
		return true;
	}
	bool allocated = true;
	for (int i = 0; i < CAM_RING_FRAMES; i++) {
		cameraB->ring[i] = malloc(CAM_FRAME_BYTES);
		if (!cameraB->ring[i]) {
			allocated = false;
		}
	}
	if (!allocated) {
		set_camera_b_ring(false);
	}
	return allocated;
}

void dual_stream_free(void)
{
	CamPort *cameraB = &g_camera.ports[CAM_PORT_B];
	if (cameraB->errorEvent) {
		svcCloseHandle(cameraB->errorEvent);
		cameraB->errorEvent = 0;
	}
	set_camera_b_ring(false);
	free(g_camera.sink);
	g_camera.sink = NULL;
	g_camera.dual = false;
	g_camera.dualDesync = false;
	g_camera.streamingPorts = CAM_PORT_MASK_A;
	g_camera.view = CAM_PORT_A;
	g_camera.hideFramesUntil = 0;
}

// Camera B goes back to YUYV while it is awake: in standby a sensor ignores the sequencer refresh. Sensor
// registers outlast an Activate cycle, so both frame lengths are written back. Brightness synchronisation
// is the module's default again.
void dual_stream_leave(void)
{
	if (!g_camera.dual) {
		return;
	}
	sensor_i2c_set_raw_output(CAM_OUTER_LEFT, false);
	sensor_i2c_write16(SENSOR_DEVICE_OUT1, SENSOR_REG_FRAME_LENGTH, g_camera.frameLinesSaved[CAM_PORT_A]);
	sensor_i2c_write16(SENSOR_DEVICE_OUT2, SENSOR_REG_FRAME_LENGTH, g_camera.frameLinesSaved[CAM_PORT_B]);
	CAMU_SetBrightnessSynchronization(true);
	dual_stream_free();
}

// The module keeps its state between sessions, so the next application gets the sensors as the module runs
// them.
void dual_stream_exit(void)
{
	sensor_i2c_mcu_write(SENSOR_DEVICE_OUT1, SENSOR_MCU_SEQ_MODE, SENSOR_SEQ_MODE_AUTO);
	sensor_i2c_mcu_write(SENSOR_DEVICE_OUT2, SENSOR_MCU_SEQ_MODE, SENSOR_SEQ_MODE_AUTO);
	dual_stream_leave();
}

// Both sensors to the full frame: the frame lengths back to back, so both likely take them on the same
// frame, then each sensor's own read mode with row skipping off. One sensor's read mode is never copied to
// the other: its mirror bits decide its Bayer order. A failure marks the pair out of step.
static Result write_full_frame_timing(void)
{
	u16 frameLines = cam_state_frame_lines();
	Result firstError = 0;
	for (int port = 0; port < CAM_PORT_COUNT; port++) {
		Result result = sensor_i2c_write16(PORT_SENSOR_DEVICES[port], SENSOR_REG_FRAME_LENGTH, frameLines);
		if (R_FAILED(result) && R_SUCCEEDED(firstError)) {
			firstError = result;
		}
	}
	for (int port = 0; port < CAM_PORT_COUNT; port++) {
		u16 readMode = g_camera.readModeDual[port];
		if (!readMode) {
			continue;
		}
		u16 fullFrameReadMode = (readMode & ~SENSOR_READ_MODE_Y_ODD_INC_MASK) | SENSOR_READ_ROWS_ALL;
		Result result =
		        sensor_i2c_write16(PORT_SENSOR_DEVICES[port], SENSOR_REG_READ_MODE, fullFrameReadMode);
		if (R_FAILED(result) && R_SUCCEEDED(firstError)) {
			firstError = result;
		}
	}
	if (R_FAILED(firstError)) {
		g_camera.dualDesync = true;
	}
	return firstError;
}

// Camera B's ring (only when it is the live view) and the sink for the other port's frames outside a
// burst. Taken at the start so the shutter never allocates.
static bool allocate_camera_b_frames(int view)
{
	if (!set_camera_b_ring(view == CAM_PORT_B)) {
		return false;
	}
	if (!g_camera.sink) {
		g_camera.sink = malloc(CAM_FRAME_BYTES);
	}
	return g_camera.sink != NULL;
}

static void select_dual_ports(int view)
{
	g_camera.dual = true; // from here on dual_stream_leave undoes it
	g_camera.dualDesync = false;
	g_camera.cameraSelect = DUAL_CAMERA_SELECT;
	g_camera.portIds[CAM_PORT_A] = PORT_CAM1;
	g_camera.portIds[CAM_PORT_B] = PORT_CAM2;
	g_camera.livePort = g_camera.portIds[view];
	g_camera.frameWidth = CAM_FRAME_WIDTH;
	g_camera.frameHeight = CAM_FRAME_HEIGHT;
}

// Configured while switched off: each call on an active camera waits about two frames. The module only
// reprograms a sensor when its size changes, so each context goes through QQVGA first.
static void configure_sensors(const CamSettings *settings, Result *firstError)
{
	Result err = *firstError;
	CAM_TRY(CAMU_Activate(SELECT_NONE));
	CAM_TRY_AS("CAMU_SetSize(sel, SIZE_QQVGA, CONTEXT_A)",
	           CAMU_SetSize(DUAL_CAMERA_SELECT, SIZE_QQVGA, CONTEXT_A));
	CAM_TRY_AS("CAMU_SetSize(sel, SIZE_QQVGA, CONTEXT_B)",
	           CAMU_SetSize(DUAL_CAMERA_SELECT, SIZE_QQVGA, CONTEXT_B));
	CAM_TRY_AS("CAMU_SetSize(sel, SIZE_VGA, CONTEXT_A)",
	           CAMU_SetSize(DUAL_CAMERA_SELECT, SIZE_VGA, CONTEXT_A));
	CAM_TRY_AS("CAMU_SetOutputFormat(sel, OUTPUT_YUV_422, CONTEXT_A)",
	           CAMU_SetOutputFormat(DUAL_CAMERA_SELECT, OUTPUT_YUV_422, CONTEXT_A));
	CAM_TRY_AS("CAMU_SetSize(sel, SIZE_VGA, CONTEXT_B)",
	           CAMU_SetSize(DUAL_CAMERA_SELECT, SIZE_VGA, CONTEXT_B));
	CAM_TRY_AS("CAMU_SetOutputFormat(sel, OUTPUT_YUV_422, CONTEXT_B)",
	           CAMU_SetOutputFormat(DUAL_CAMERA_SELECT, OUTPUT_YUV_422, CONTEXT_B));
	CAM_TRY_AS("CAMU_SwitchContext(sel, CONTEXT_A)", CAMU_SwitchContext(DUAL_CAMERA_SELECT, CONTEXT_A));
	CamSettings allUnset; // every control differs from it, so every control is applied
	CamSettings controls = *settings;
	memset(&allUnset, CAM_SETTINGS_UNSET_BYTE, sizeof allUnset);
	// A fixed rate: with a variable one the module's AE could stretch each sensor's frames by its own amount
	// and lose the vsync alignment for good.
	controls.frameRate = FRAME_RATE_30;
	// The camera service's AE and AWB stay off: in raw the app sets exposure and colour.
	controls.autoExposure = 0;
	controls.awbLock = 1;
	Result controlsResult = cam_config_apply_image_controls(DUAL_CAMERA_SELECT, &controls, &allUnset);
	if (R_SUCCEEDED(err)) {
		err = controlsResult;
	}
	CAM_TRY_AS("CAMU_FlipImage(sel, FLIP_NONE, CONTEXT_A)",
	           CAMU_FlipImage(DUAL_CAMERA_SELECT, FLIP_NONE, CONTEXT_A));
	CAM_TRY_AS("CAMU_FlipImage(sel, FLIP_NONE, CONTEXT_B)",
	           CAMU_FlipImage(DUAL_CAMERA_SELECT, FLIP_NONE, CONTEXT_B));
	CAM_TRY_AS("CAMU_Activate(sel)", CAMU_Activate(DUAL_CAMERA_SELECT));
	// With it on, the module copies one outer sensor's exposure to the other, which would fight ours.
	CAM_TRY(CAMU_SetBrightnessSynchronization(false));
	*firstError = err;
}

// Each sensor's own frame length, given back when dual ends.
static void save_sensor_frame_lengths(void)
{
	for (int port = 0; port < CAM_PORT_COUNT; port++) {
		u16 frameLines = 0;
		Result result = sensor_i2c_read16(PORT_SENSOR_DEVICES[port], SENSOR_REG_FRAME_LENGTH, &frameLines);
		bool plausible = frameLines > SAVED_FRAME_LINES_MIN && frameLines < SAVED_FRAME_LINES_MAX;
		if (R_SUCCEEDED(result) && plausible && frameLines != DUAL_FRAME_LINES) {
			g_camera.frameLinesSaved[port] = frameLines;
		}
	}
}

// Raw on both once they are awake: a sensor in standby ignores the sequencer refresh.
static Result switch_sensors_to_raw(void)
{
	Result result = sensor_i2c_set_raw_output(CAM_OUTER_RIGHT, true);
	if (R_SUCCEEDED(result)) {
		result = sensor_i2c_set_raw_output(CAM_OUTER_LEFT, true);
	}
	return result;
}

// Each sensor's read mode as it is now (0: unread), kept for its mirror bits.
static void read_sensor_read_modes(void)
{
	for (int port = 0; port < CAM_PORT_COUNT; port++) {
		u16 readMode = 0;
		Result result = sensor_i2c_read16(PORT_SENSOR_DEVICES[port], SENSOR_REG_READ_MODE, &readMode);
		g_camera.readModeDual[port] = R_SUCCEEDED(result) ? readMode : 0;
	}
}

// Both ports cut the 648 x 488 raw readout to the full frame.
static void set_up_port_transfers(int view, Result *firstError)
{
	Result err = *firstError;
	for (int port = 0; port < CAM_PORT_COUNT; port++) {
		cam_config_raw_trim(g_camera.portIds[port], CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, false, &err);
	}
	u32 maxBytes = 0;
	CAM_TRY_AS("CAMU_GetMaxBytes(&maxBytes, CP_STILL_W, CP_STILL_H)",
	           CAMU_GetMaxBytes(&maxBytes, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT));
	g_camera.transferBytes = maxBytes;
	CAM_TRY_AS("CAMU_SetTransferBytes(S.port, maxBytes, CP_STILL_W, CP_STILL_H)",
	           CAMU_SetTransferBytes(g_camera.livePort, maxBytes, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT));
	// The other port: dual_burst_take starts it for a shot.
	u32 otherPortId = g_camera.portIds[cam_state_other_port(view)];
	CAM_TRY_AS("CAMU_SetTransferBytes(S.portId[view ^ 1], maxBytes, CP_STILL_W, CP_STILL_H)",
	           CAMU_SetTransferBytes(otherPortId, maxBytes, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT));
	CAM_TRY(CAMU_ClearBuffer(PORT_BOTH));
	*firstError = err;
}

// Fresh buffer error events for both ports.
static void open_error_events(Result *firstError)
{
	Result err = *firstError;
	for (int c = 0; c < CAM_PORT_COUNT; c++) {
		if (g_camera.ports[c].errorEvent) {
			svcCloseHandle(g_camera.ports[c].errorEvent);
			g_camera.ports[c].errorEvent = 0;
		}
		CAM_TRY_AS("CAMU_GetBufferErrorInterruptEvent(&S.P[c].errEvt, S.portId[c])",
		           CAMU_GetBufferErrorInterruptEvent(&g_camera.ports[c].errorEvent, g_camera.portIds[c]));
		if (g_camera.ports[c].errorEvent) {
			svcClearEvent(g_camera.ports[c].errorEvent); // an overrun from an earlier capture is no news
		}
	}
	*firstError = err;
}

// With both sensors raw: vsync aligned, the same timing on both, the ports set up and the live view's port
// started. The raw switch restarts each sensor, so the alignment comes after it. Returns StartCapture's
// result.
static Result start_live_port(int view, Result *firstError)
{
	Result err = *firstError;
	CAM_TRY(CAMU_SynchronizeVsyncTiming(SELECT_OUT1, SELECT_OUT2));
	read_sensor_read_modes();
	write_full_frame_timing();
	set_up_port_transfers(view, &err);
	open_error_events(&err);
	*firstError = err;
	return CAMU_StartCapture(g_camera.livePort);
}

bool dual_stream_start(const CamSettings *settings, Result *firstError)
{
	bool restarting = g_camera.dual; // the sensors' own frame lengths are saved already
	// The live view's port: it streams alone.
	int view = settings->camera == CAM_OUTER_LEFT ? CAM_PORT_B : CAM_PORT_A;
	if (!allocate_camera_b_frames(view)) {
		// Leave a restarted dual stream while the sensors are awake: the one-camera start must not begin
		// from dual's state.
		dual_stream_leave();
		set_camera_b_ring(false); // a first start may have the ring without the sink
		g_camera.dualBroken = true;
		*firstError = 0;
		return false;
	}
	select_dual_ports(view);
	if (!restarting) {
		g_camera.frameLinesSaved[CAM_PORT_A] = SENSOR_FULL_FRAME_LINES;
		g_camera.frameLinesSaved[CAM_PORT_B] = SENSOR_FULL_FRAME_LINES;
	}
	Result err = 0;
	configure_sensors(settings, &err);
	if (!restarting) {
		save_sensor_frame_lengths();
	}
	Result rawResult = switch_sensors_to_raw();
	Result startResult = rawResult;
	g_camera.dualFrameLines = dual_stream_frame_lines(settings);
	g_camera.fastViewOn = false; // the dual sensors run full frames all the time
	if (R_SUCCEEDED(rawResult)) {
		startResult = start_live_port(view, &err);
	}
	if (R_FAILED(startResult)) {
		CAMU_StopCapture(PORT_BOTH);
		dual_stream_leave();
		g_camera.dualBroken = true; // retried only when the mode is chosen again
		*firstError = err;
		return false;
	}
	g_camera.streamingPorts = 1u << view;
	g_camera.view = view;
	g_camera.rawActive = true;
	// The restart history is kept: a failing burst restarts the stream through here.
	for (int port = 0; port < CAM_PORT_COUNT; port++) {
		g_camera.ports[port].frames = 0;
		g_camera.ports[port].fps = 0;
	}
	g_camera.lastError = err;
	g_camera.settings = *settings;
	g_camera.running = true;
	capture_thread_resume();
	// The exposure goes on after StartCapture, with receives armed: set before it, the sensors' own AE and
	// AWB come back on.
	sensor_exposure_apply(settings);
	sensor_exposure_ask_guard(); // a look once the start's window has passed
	*firstError = err;
	return true;
}

void dual_stream_set_wanted(bool on)
{
	if (!on) {
		// Leaving the mode gives dual another try, with a clean history, when it is chosen again.
		g_camera.dualBroken = false;
		g_camera.burstFails = 0;
		g_camera.dualRestartCount = 0;
	}
	g_camera.dualWanted = on;
}

bool dual_stream_active(void)
{
	return g_camera.running && g_camera.dual;
}

bool dual_stream_broken(void)
{
	return g_camera.running && g_camera.dual && g_camera.dualBroken;
}

bool dual_stream_desync(void)
{
	return g_camera.dualDesync;
}
