#include "still_capture.h"
#include <3ds.h>
#include <string.h>
#include "cam_config.h"
#include "cam_settings.h"
#include "cam_state.h"
#include "cam_stream.h"
#include "capture_thread.h"
#include "dual_stream.h"
#include "dual_stream_internal.h"
#include "fast_view.h"
#include "sensor_exposure.h"
#include "sensor_i2c.h"
#include "time_units.h"

#define FRAME_TIMEOUT_NS (2000 * NANOSECONDS_PER_MILLISECOND)

// YUV 3D pair: frames dropped while auto exposure settles; the pair is the frame after them.
#define PAIR_SETTLE_FRAMES 4

// Raw burst
#define BURST_SETTLE_FRAMES 1 // the first frame after a start is dropped
#define BURST_MAX_KEPT_FRAMES (CAM_BURST_MAX_FRAMES - BURST_SETTLE_FRAMES)
#define EXPOSURE_GUARD_CHECKS 2
#define GUARD_CHECK_INTERVAL_NS (80 * NANOSECONDS_PER_MILLISECOND)
#define FULL_FRAME_READ_MODE 0x0025 // every row read; used when the stream recorded no read mode
#define FRAME_WAIT_MARGIN_SECONDS 1.5f // how much longer than its exposure a burst frame may take
#define TICKS_PER_MICROSECOND (CPU_TICKS_PER_MSEC / (double)MICROSECONDS_PER_MILLISECOND)

static const u32 BURST_PORTS[RAW_SHOT_CAMERA_COUNT] = {PORT_CAM1, PORT_CAM2};

// The cameras, ports and sensors of one raw burst, and the exposure it writes to them.
typedef struct {
	bool stereo;
	int cameraCount;
	int cameras[RAW_SHOT_CAMERA_COUNT]; // CAM_* ids of camera A and camera B
	u8 devices[RAW_SHOT_CAMERA_COUNT]; // their sensors' I2C devices
	u32 cameraSelect;
	u32 port;
	u8 *const *buffers[RAW_SHOT_CAMERA_COUNT]; // each camera's frame buffers
	int receiveCount; // frames each camera delivers, settle frames included
	u16 lines;
	u16 gain;
	u16 frameLines;
	u32 maxBytes;
	Handle errorEvents[RAW_SHOT_CAMERA_COUNT];
} RawBurst;

// When each camera's burst frames finished, in system ticks.
typedef struct {
	int frameCount[RAW_SHOT_CAMERA_COUNT];
	u64 doneTicks[RAW_SHOT_CAMERA_COUNT][CAM_BURST_MAX_FRAMES];
} BurstTimeline;

// The shutter sound is due once the frames of the next raw burst are in: from then on the camera may move.
static bool g_lateSoundDue;

// A wait that times out returns 0x09401BFE, which R_SUCCEEDED accepts: only 0 means signaled.
static bool event_signaled(Handle event)
{
	return event && svcWaitSynchronization(event, 0) == 0;
}

// Receives into the same buffer until the last frame, so the frames while the sensor settles are dropped.
static Result receive_still_frame(u16 *destination, int skipFrames, u32 maxBytes)
{
	Result result = 0;
	for (int i = 0; i <= skipFrames; i++) {
		Handle frameEvent;
		result = CAMU_SetReceiving(&frameEvent, destination, g_camera.livePort, CAM_FRAME_BYTES,
		                           (s16)maxBytes);
		if (R_FAILED(result)) {
			break;
		}
		result = svcWaitSynchronization(frameEvent, FRAME_TIMEOUT_NS);
		svcCloseHandle(frameEvent);
		if (result != 0) {
			if (R_SUCCEEDED(result)) {
				result = CAM_ERROR_TIMED_OUT;
			}
			CAMU_ClearBuffer(g_camera.livePort);
			break;
		}
	}
	return result;
}

Result still_capture_yuv_photo(u16 *destination, int skipFrames)
{
	if (!g_camera.running) {
		return CAM_ERROR_REFUSED;
	}
	Result err = 0;
	g_camera.lastErrorCall = NULL;

	capture_thread_pause();
	CAMU_StopCapture(g_camera.livePort);
	CAM_TRY_AS("CAMU_SetTrimming(S.port, false)", CAMU_SetTrimming(g_camera.livePort, false));
	CAM_TRY_AS("CAMU_SwitchContext(S.sel, CONTEXT_B)", CAMU_SwitchContext(g_camera.cameraSelect, CONTEXT_B));
	u32 maxBytes = 0;
	CAM_TRY_AS("CAMU_GetMaxBytes(&maxBytes, CP_STILL_W, CP_STILL_H)",
	           CAMU_GetMaxBytes(&maxBytes, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT));
	CAM_TRY_AS("CAMU_SetTransferBytes(S.port, maxBytes, CP_STILL_W, CP_STILL_H)",
	           CAMU_SetTransferBytes(g_camera.livePort, maxBytes, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT));
	CAM_TRY_AS("CAMU_ClearBuffer(S.port)", CAMU_ClearBuffer(g_camera.livePort));
	CAM_TRY_AS("CAMU_StartCapture(S.port)", CAMU_StartCapture(g_camera.livePort));
	Result result = 0;
	if (R_SUCCEEDED(err)) {
		result = receive_still_frame(destination, skipFrames, maxBytes);
	}
	CAMU_StopCapture(g_camera.livePort);
	CAM_TRY_AS("CAMU_SwitchContext(S.sel, CONTEXT_A)", CAMU_SwitchContext(g_camera.cameraSelect, CONTEXT_A));

	cam_config_stream_trim(&err);
	CAM_TRY_AS("CAMU_SetTransferBytes(S.port, S.transfer, S.w, S.h)",
	           CAMU_SetTransferBytes(g_camera.livePort, g_camera.transferBytes, g_camera.frameWidth,
	                                 g_camera.frameHeight));
	CAM_TRY_AS("CAMU_ClearBuffer(S.port)", CAMU_ClearBuffer(g_camera.livePort));
	CAM_TRY_AS("CAMU_StartCapture(S.port)", CAMU_StartCapture(g_camera.livePort));
	capture_thread_resume();
	if (R_SUCCEEDED(result)) {
		result = err;
	}
	g_camera.lastError = result;
	return result;
}

// Steps that use CAM_TRY take the first error so far as `err` and return it.

// Configures cameras while they are off, since every call on an active camera waits about two frames. The
// module reprograms a sensor only when its size changes, so a different size comes first. All-unset previous
// controls make every image control apply.
static Result configure_inactive_cameras(u32 cameraSelect, const CamSettings *controls, Result err)
{
	CAM_TRY(CAMU_Activate(SELECT_NONE));
	CAM_TRY_AS("CAMU_SetSize(sel, SIZE_QQVGA, CONTEXT_A)", CAMU_SetSize(cameraSelect, SIZE_QQVGA, CONTEXT_A));
	CAM_TRY_AS("CAMU_SetSize(sel, SIZE_VGA, CONTEXT_A)", CAMU_SetSize(cameraSelect, SIZE_VGA, CONTEXT_A));
	CAM_TRY_AS("CAMU_SetOutputFormat(sel, OUTPUT_YUV_422, CONTEXT_A)",
	           CAMU_SetOutputFormat(cameraSelect, OUTPUT_YUV_422, CONTEXT_A));
	CAM_TRY_AS("CAMU_SwitchContext(sel, CONTEXT_A)", CAMU_SwitchContext(cameraSelect, CONTEXT_A));
	CamSettings allUnset;
	memset(&allUnset, CAM_SETTINGS_UNSET_BYTE, sizeof allUnset);
	Result controlsResult = cam_config_apply_image_controls(cameraSelect, controls, &allUnset);
	if (R_SUCCEEDED(err)) {
		err = controlsResult;
	}
	return err;
}

// Receives one frame from each outer camera, again and again while the exposure settles; the last pair stays.
static Result receive_yuv_pair(u8 *outA, u8 *outB, u32 maxBytes)
{
	Result result = 0;
	for (int i = 0; i <= PAIR_SETTLE_FRAMES && R_SUCCEEDED(result); i++) {
		Handle eventA = 0;
		Handle eventB = 0;
		result = CAMU_SetReceiving(&eventA, outA, PORT_CAM1, CAM_FRAME_BYTES, (s16)maxBytes);
		if (R_SUCCEEDED(result)) {
			result = CAMU_SetReceiving(&eventB, outB, PORT_CAM2, CAM_FRAME_BYTES, (s16)maxBytes);
		}
		if (R_SUCCEEDED(result) && svcWaitSynchronization(eventA, FRAME_TIMEOUT_NS) != 0) {
			result = CAM_ERROR_TIMED_OUT;
		}
		if (R_SUCCEEDED(result) && svcWaitSynchronization(eventB, FRAME_TIMEOUT_NS) != 0) {
			result = CAM_ERROR_TIMED_OUT;
		}
		if (eventA) {
			svcCloseHandle(eventA);
		}
		if (eventB) {
			svcCloseHandle(eventB);
		}
	}
	return result;
}

Result still_capture_yuv_pair(const CamSettings *settings, u8 *outA, u8 *outB)
{
	if (!g_camera.initialized) {
		return CAM_ERROR_REFUSED;
	}
	CamSettings controls = *settings;
	cam_settings_clamp(&controls);
	Result err = 0;
	g_camera.lastErrorCall = NULL;
	const u32 cameraSelect = SELECT_OUT1 | SELECT_OUT2;

	sensor_exposure_park();
	capture_thread_pause();
	CAMU_StopCapture(PORT_BOTH);
	g_camera.running = false; // the stream starts again at the end
	// Every control on both cameras, the EV bias too, so they record alike.
	err = configure_inactive_cameras(cameraSelect, &controls, err);
	CAM_TRY_AS("CAMU_Activate(sel)", CAMU_Activate(cameraSelect));
	CAM_TRY(CAMU_SetBrightnessSynchronization(true));
	CAM_TRY(CAMU_SynchronizeVsyncTiming(SELECT_OUT1, SELECT_OUT2));
	CAM_TRY(CAMU_SetTrimming(PORT_CAM1, false));
	CAM_TRY(CAMU_SetTrimming(PORT_CAM2, false));
	u32 maxBytes = 0;
	CAM_TRY_AS("CAMU_GetMaxBytes(&maxBytes, CP_STILL_W, CP_STILL_H)",
	           CAMU_GetMaxBytes(&maxBytes, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT));
	CAM_TRY_AS("CAMU_SetTransferBytes(PORT_BOTH, maxBytes, CP_STILL_W, CP_STILL_H)",
	           CAMU_SetTransferBytes(PORT_BOTH, maxBytes, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT));
	CAM_TRY(CAMU_ClearBuffer(PORT_BOTH));
	CAM_TRY(CAMU_StartCapture(PORT_BOTH));

	Result result = 0;
	if (R_SUCCEEDED(err)) {
		result = receive_yuv_pair(outA, outB, maxBytes);
	}
	CAMU_StopCapture(PORT_BOTH);
	cam_stream_start(&g_camera.settings); // back to the preview
	if (R_FAILED(result)) {
		return result;
	}
	return err;
}

// Raw bursts and 3D pairs: both outer sensors in raw at once, frame-synchronised, with one exposure written
// to both.

void still_capture_set_late_sound(bool enabled)
{
	g_lateSoundDue = enabled;
}

bool still_capture_take_late_sound(void)
{
	bool due = g_lateSoundDue;
	g_lateSoundDue = false;
	return due;
}

// The camera module may switch a sensor's AE and AWB back on shortly after a start. Turns them off again and
// puts back a fine integration time that is not ours.
static void guard_sensor_exposure(u8 device, u16 lines, u16 gain)
{
	u16 fineTime = 0;
	Result fineResult = sensor_i2c_read16(device, SENSOR_REG_FINE_TIME, &fineTime);
	if (R_SUCCEEDED(fineResult) && fineTime != sensor_exposure_fine_time()) {
		sensor_i2c_write16(device, SENSOR_REG_FINE_TIME, sensor_exposure_fine_time());
	}
	u16 sequencerMode = 0;
	Result modeResult = sensor_i2c_mcu_read(device, SENSOR_MCU_SEQ_MODE, &sequencerMode);
	if (R_FAILED(modeResult) || !(sequencerMode & SENSOR_SEQ_AE_AWB)) {
		return;
	}
	sensor_exposure_write_device(device, lines, gain);
}

// Picks the cameras, ports and sensors of a burst. Stereo needs camera B's buffers and an outer camera A.
static void plan_raw_burst(RawBurst *burst, const CamSettings *settings, int frames, bool stereo,
                           u8 *const *outA, u8 *const *outB)
{
	int cameraA = settings->camera == CAM_INNER ? CAM_INNER : CAM_OUTER_RIGHT;
	bool bothCameras = stereo && cameraA != CAM_INNER && outB != NULL;
	int keptFrames = frames < BURST_MAX_KEPT_FRAMES ? frames : BURST_MAX_KEPT_FRAMES;
	*burst = (RawBurst){
	        .stereo = bothCameras,
	        .cameraCount = bothCameras ? RAW_SHOT_CAMERA_COUNT : 1,
	        .cameras = {cameraA, CAM_OUTER_LEFT},
	        .devices = {SENSOR_I2C_DEVICES[cameraA], SENSOR_I2C_DEVICES[CAM_OUTER_LEFT]},
	        .cameraSelect = bothCameras ? (SELECT_OUT1 | SELECT_OUT2) : CAM_SERVICE_SELECT[cameraA],
	        .port = bothCameras ? PORT_BOTH : PORT_CAM1,
	        .buffers = {outA, outB},
	        .receiveCount = BURST_SETTLE_FRAMES + keptFrames,
	};
}

// Stops the live stream and leaves the fast view and the dual stream, so both sensors are free.
static void stop_stream_for_burst(void)
{
	sensor_exposure_park(); // the burst writes its own time once the sensors are on
	capture_thread_pause();
	CAMU_StopCapture(PORT_BOTH);
	// Full frames again while the ports are stopped: a mode switch under a running port breaks its receive.
	fast_view_set_sensor(false);
	g_camera.running = false;
	// Leave a dual stream while the sensors are awake. The stream start at the end brings it back.
	dual_stream_leave();
	g_camera.rawActive = false;
}

// The same controls on both sensors with the camera service's AE and AWB off, then raw output once the
// sensors are awake (a sensor in standby ignores the switch).
static Result configure_burst_cameras(const RawBurst *burst, const CamSettings *settings, Result err)
{
	u32 cameraSelect = burst->cameraSelect;
	CamSettings controls = *settings;
	// A fixed 30 fps: should the module's AE run before the exposure is written, it must not stretch a frame.
	controls.frameRate = FRAME_RATE_30;
	controls.autoExposure = 0;
	controls.awbLock = 1;
	err = configure_inactive_cameras(cameraSelect, &controls, err);
	// Raw frames keep the sensor's own orientation. Set while the cameras are still off.
	CAM_TRY_AS("CAMU_FlipImage(sel, FLIP_NONE, CONTEXT_A)",
	           CAMU_FlipImage(cameraSelect, FLIP_NONE, CONTEXT_A));
	CAM_TRY_AS("CAMU_FlipImage(sel, FLIP_NONE, CONTEXT_B)",
	           CAMU_FlipImage(cameraSelect, FLIP_NONE, CONTEXT_B));
	CAM_TRY_AS("CAMU_Activate(sel)", CAMU_Activate(cameraSelect));
	for (int i = 0; i < burst->cameraCount && R_SUCCEEDED(err); i++) {
		Result rawResult = sensor_i2c_set_raw_output(burst->cameras[i], true);
		if (R_FAILED(rawResult)) {
			err = rawResult;
			g_camera.lastErrorCall = "cp_set_raw"; // the raw switch as the info page and the toasts name it
		}
	}
	// After the raw switch: it restarts each sensor, which undoes an earlier synchronisation.
	if (burst->stereo) {
		CAM_TRY(CAMU_SynchronizeVsyncTiming(SELECT_OUT1, SELECT_OUT2));
	}
	return err;
}

// The fast live view shortens camera A's frame and skips rows, and an integration time cannot outlast its
// frame. So every sensor gets the read mode and frame length used outside the fast view.
static void set_full_frame_timing(RawBurst *burst)
{
	u16 readMode = g_camera.readModeNormal ? g_camera.readModeNormal : FULL_FRAME_READ_MODE;
	burst->frameLines = g_camera.frameLinesNormal ? g_camera.frameLinesNormal : SENSOR_FULL_FRAME_LINES;
	u16 rowSkipping = readMode & SENSOR_READ_MODE_Y_ODD_INC_MASK;
	for (int i = 0; i < burst->cameraCount; i++) {
		u16 currentMode = 0;
		sensor_i2c_read16(burst->devices[i], SENSOR_REG_READ_MODE, &currentMode);
		// Only the row skipping changes; the mirroring stays.
		u16 newMode = (currentMode & ~SENSOR_READ_MODE_Y_ODD_INC_MASK) | rowSkipping;
		sensor_i2c_write16(burst->devices[i], SENSOR_REG_READ_MODE, newMode);
		sensor_i2c_write16(burst->devices[i], SENSOR_REG_FRAME_LENGTH, burst->frameLines);
	}
}

// Full frames on every port of the burst and an event for each port's buffer errors, then the capture
// starts.
static Result start_burst_ports(RawBurst *burst, Result err)
{
	for (int i = 0; i < burst->cameraCount; i++) {
		cam_config_raw_trim(BURST_PORTS[i], CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, false, &err);
	}
	u32 port = burst->port;
	u32 maxBytes = 0;
	CAM_TRY_AS("CAMU_GetMaxBytes(&maxBytes, CP_STILL_W, CP_STILL_H)",
	           CAMU_GetMaxBytes(&maxBytes, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT));
	CAM_TRY_AS("CAMU_SetTransferBytes(port, maxBytes, CP_STILL_W, CP_STILL_H)",
	           CAMU_SetTransferBytes(port, maxBytes, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT));
	CAM_TRY(CAMU_ClearBuffer(port));
	burst->maxBytes = maxBytes;
	for (int i = 0; i < burst->cameraCount; i++) {
		CAMU_GetBufferErrorInterruptEvent(&burst->errorEvents[i], BURST_PORTS[i]);
	}
	CAM_TRY(CAMU_StartCapture(port));
	return err;
}

// Arms each camera's receive into its first frame buffer.
static Result arm_first_frames(const RawBurst *burst, Handle frameEvents[RAW_SHOT_CAMERA_COUNT])
{
	Result result = 0;
	for (int i = 0; i < burst->cameraCount && R_SUCCEEDED(result); i++) {
		result = CAMU_SetReceiving(&frameEvents[i], burst->buffers[i][0], BURST_PORTS[i], CAM_FRAME_BYTES,
		                           (s16)burst->maxBytes);
	}
	return result;
}

// Closes the receive events still open.
static void close_events(Handle events[RAW_SHOT_CAMERA_COUNT])
{
	for (int i = 0; i < RAW_SHOT_CAMERA_COUNT; i++) {
		if (events[i]) {
			svcCloseHandle(events[i]);
		}
	}
}

// Writes the exposure while receives are pending, since the sensors' AE and AWB come back on at a start, and
// guards it for a few frames. The pauses overrun the ports, so they are stopped and cleared afterwards.
static Result settle_burst_exposure(const RawBurst *burst, Result err)
{
	Handle frameEvents[RAW_SHOT_CAMERA_COUNT] = {0, 0};
	arm_first_frames(burst, frameEvents);
	for (int i = 0; i < burst->cameraCount; i++) {
		sensor_exposure_write_device(burst->devices[i], burst->lines, burst->gain);
	}
	for (int check = 0; check < EXPOSURE_GUARD_CHECKS; check++) {
		svcSleepThread(GUARD_CHECK_INTERVAL_NS);
		for (int i = 0; i < burst->cameraCount; i++) {
			guard_sensor_exposure(burst->devices[i], burst->lines, burst->gain);
		}
	}
	u32 port = burst->port;
	CAMU_StopCapture(port); // also cancels the receives still pending
	close_events(frameEvents);
	CAM_TRY(CAMU_ClearBuffer(port));
	for (int i = 0; i < RAW_SHOT_CAMERA_COUNT; i++) {
		if (burst->errorEvents[i]) {
			svcClearEvent(burst->errorEvents[i]);
		}
	}
	return err;
}

// Reads the timing each sensor runs with: it must match between the sensors.
static void read_sensor_timing(const RawBurst *burst, RawShotInfo *info)
{
	for (int i = 0; i < burst->cameraCount; i++) {
		for (int slot = 0; slot < RAW_SHOT_TIMING_COUNT; slot++) {
			sensor_i2c_read16(burst->devices[i], SENSOR_TIMING_REGISTERS[slot], &info->timing[i][slot]);
		}
	}
}

// How long to wait for one burst frame: its exposure plus a margin.
static s64 burst_frame_timeout_ns(u16 lines)
{
	float lineMicroseconds = sensor_exposure_line_microseconds();
	float exposureSeconds = lines * lineMicroseconds / (float)MICROSECONDS_PER_SECOND;
	return (s64)((exposureSeconds + FRAME_WAIT_MARGIN_SECONDS) * (float)NANOSECONDS_PER_SECOND);
}

// Receives the burst with no sensor calls. Each port is re-armed the moment its frame lands, since a receive
// armed late overruns the port. The settle frame's buffer is reused for the first kept frame.
static Result receive_burst_frames(const RawBurst *burst, s64 frameTimeoutNs, Result err, RawShotInfo *info,
                                   BurstTimeline *timeline)
{
	Handle frameEvents[RAW_SHOT_CAMERA_COUNT] = {0, 0};
	Result result = 0;
	if (R_SUCCEEDED(err)) {
		result = arm_first_frames(burst, frameEvents);
	}
	while (R_SUCCEEDED(err) && R_SUCCEEDED(result)) {
		Handle waitEvents[RAW_SHOT_CAMERA_COUNT];
		int waitCameras[RAW_SHOT_CAMERA_COUNT];
		int waitCount = 0;
		for (int i = 0; i < burst->cameraCount; i++) {
			if (frameEvents[i]) {
				waitEvents[waitCount] = frameEvents[i];
				waitCameras[waitCount] = i;
				waitCount++;
			}
		}
		if (waitCount == 0) {
			break; // every camera has all its frames
		}
		s32 signaledIndex = -1;
		Result waitResult =
		        svcWaitSynchronizationN(&signaledIndex, waitEvents, waitCount, false, frameTimeoutNs);
		if (waitResult != 0 || signaledIndex < 0 || signaledIndex >= waitCount) {
			result = CAM_ERROR_TIMED_OUT;
			break;
		}
		int camera = waitCameras[signaledIndex];
		timeline->doneTicks[camera][timeline->frameCount[camera]] = svcGetSystemTick();
		svcCloseHandle(frameEvents[camera]);
		frameEvents[camera] = 0;
		info->received[camera]++;
		timeline->frameCount[camera]++;
		int frameCount = timeline->frameCount[camera];
		if (frameCount < burst->receiveCount) {
			int slot = frameCount < BURST_SETTLE_FRAMES ? 0 : frameCount - BURST_SETTLE_FRAMES;
			result = CAMU_SetReceiving(&frameEvents[camera], burst->buffers[camera][slot],
			                           BURST_PORTS[camera], CAM_FRAME_BYTES, (s16)burst->maxBytes);
		}
	}
	// Stopped the moment the frames are in: a port without a pending receive overruns at its next frame.
	CAMU_StopCapture(burst->port);
	close_events(frameEvents);
	return result;
}

// Notes which ports overran, then clears the ports and their error events so nothing is left for the stream.
static void collect_buffer_errors(const RawBurst *burst, RawShotInfo *info)
{
	for (int i = 0; i < RAW_SHOT_CAMERA_COUNT; i++) {
		info->bufferError[i] = event_signaled(burst->errorEvents[i]);
	}
	CAMU_ClearBuffer(PORT_BOTH);
	for (int i = 0; i < RAW_SHOT_CAMERA_COUNT; i++) {
		if (burst->errorEvents[i]) {
			svcClearEvent(burst->errorEvents[i]);
			svcCloseHandle(burst->errorEvents[i]);
		}
	}
}

// How far apart the two cameras' frames finished. Frame-synchronised sensors finish at about the same time.
static void measure_pair_skew(const RawBurst *burst, const BurstTimeline *timeline, RawShotInfo *info)
{
	double skewSumMs = 0;
	double frameTicks = burst->frameLines * sensor_exposure_line_microseconds() * TICKS_PER_MICROSECOND;
	int comparedPairs = 0;
	const u64 *ticksA = timeline->doneTicks[RAW_SHOT_CAMERA_A];
	const u64 *ticksB = timeline->doneTicks[RAW_SHOT_CAMERA_B];
	int pairCount = timeline->frameCount[RAW_SHOT_CAMERA_A];
	if (timeline->frameCount[RAW_SHOT_CAMERA_B] < pairCount) {
		pairCount = timeline->frameCount[RAW_SHOT_CAMERA_B];
	}
	for (int i = BURST_SETTLE_FRAMES; i < pairCount; i++) {
		u64 skewTicks = ticksA[i] > ticksB[i] ? ticksA[i] - ticksB[i] : ticksB[i] - ticksA[i];
		double skewMs = (double)skewTicks / CPU_TICKS_PER_MSEC;
		skewSumMs += skewMs;
		if (skewMs > info->skewMaxMs) {
			info->skewMaxMs = (float)skewMs;
		}
		if (skewTicks < frameTicks / CAM_PAIR_WINDOW_DIVISOR) {
			info->pairMask |= 1u << (i - BURST_SETTLE_FRAMES);
			info->paired++;
		}
		comparedPairs++;
	}
	if (comparedPairs > 0) {
		info->skewMs = (float)(skewSumMs / comparedPairs);
	}
}

// What the sensors ended with, for the caller.
static void read_back_sensors(const RawBurst *burst, RawShotInfo *info)
{
	for (int i = 0; i < burst->cameraCount; i++) {
		u8 device = burst->devices[i];
		sensor_i2c_mcu_read(device, SENSOR_MCU_SEQ_MODE, &info->sequencerMode[i]);
		sensor_i2c_read16(device, SENSOR_REG_COARSE_TIME, &info->linesBack[i]);
	}
}

Result still_capture_raw_burst(const CamSettings *settings, int frames, bool stereo, u8 *const *outA,
                               u8 *const *outB, RawShotInfo *info)
{
	RawShotInfo unusedInfo;
	if (!info) {
		info = &unusedInfo;
	}
	memset(info, 0, sizeof *info);
	bool playSound = still_capture_take_late_sound(); // taken first, so a failed capture clears it too
	if (!g_camera.initialized || frames < 1 || !sensor_i2c_has_session() || !g_camera.rawActive) {
		return CAM_ERROR_REFUSED;
	}
	CamSettings clamped = *settings;
	cam_settings_clamp(&clamped);
	Result err = 0;
	g_camera.lastErrorCall = NULL;
	RawBurst burst;
	plan_raw_burst(&burst, &clamped, frames, stereo, outA, outB);
	// The burst's exposure: the manual time, or else the time and gain the live view runs with.
	sensor_exposure_metered(&clamped, &burst.lines, &burst.gain);
	info->lines = burst.lines;
	info->cameraLines[RAW_SHOT_CAMERA_A] = burst.lines;
	info->cameraLines[RAW_SHOT_CAMERA_B] = burst.lines;
	info->gain = burst.gain;

	stop_stream_for_burst();
	err = configure_burst_cameras(&burst, &clamped, err);
	set_full_frame_timing(&burst);
	err = start_burst_ports(&burst, err);
	err = settle_burst_exposure(&burst, err);
	read_sensor_timing(&burst, info);

	BurstTimeline timeline = {0};
	s64 frameTimeoutNs = burst_frame_timeout_ns(burst.lines);
	u32 port = burst.port;
	CAM_TRY(CAMU_StartCapture(port));
	Result result = receive_burst_frames(&burst, frameTimeoutNs, err, info, &timeline);
	collect_buffer_errors(&burst, info);
	if (playSound) {
		CAMU_PlayShutterSound(SHUTTER_SOUND_TYPE_NORMAL); // the frames are in: the camera may move
	}
	if (burst.stereo) {
		measure_pair_skew(&burst, &timeline, info);
	}
	read_back_sensors(&burst, info);
	if (burst.stereo) {
		// Camera B back to YUV. The stream start sets camera A up again.
		sensor_i2c_set_raw_output(CAM_OUTER_LEFT, false);
	}
	// A sensor's next start waits for a first frame as long as its integration time.
	sensor_exposure_park_devices(burst.devices, burst.cameraCount, burst.lines);
	cam_stream_start(&g_camera.settings); // back to the preview
	if (R_FAILED(result)) {
		return result;
	}
	return err;
}
