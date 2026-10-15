#include "sensor_exposure.h"
#include <3ds.h>
#include <math.h>
#include <string.h>
#include "cam_settings.h"
#include "cam_state.h"
#include "fast_view.h"
#include "math_util.h"
#include "sensor_i2c.h"
#include "time_units.h"

// Pro Cam writes the exposure straight to the sensor's core registers. The sensor's own AE and AWB stay off:
// while they run they rewrite those registers every frame.

// The light is linear in lines * LINE_PIXEL_CLOCKS + fine time.
#define LINE_PIXEL_CLOCKS 842 // LINE_LENGTH_PCK as the module programs it on all three sensors

// Fine times of 100 pixel clocks and below stop the sensor's frames, and above about 780 the light jumps by
// one and a half lines. Every fine time written stays within these limits.
#define FINE_TIME_MIN 150
#define FINE_TIME_MAX 770

// AE_R9: while the sensor's AWB runs, its MCU copies this time into the coarse time every frame.
#define MCU_AE_R9 0x2222

#define GAIN_SHORTFALL_PER_CODE 9.55e-5f // the real gain falls short of code / 8 by this much per code
#define ISO_AT_GAIN_X1 100

// The sensor's AE and AWB drivers stop within a frame or two.
#define SEQUENCER_STOP_WAIT_NS (80 * NANOSECONDS_PER_MILLISECOND)

// Auto exposure's frames last at most this long, as far as the stall limit is concerned.
#define AUTO_LONGEST_SECONDS 1.0f

// The line time until the running stream gives a better one.
#define START_LINE_MICROSECONDS 64.5f

// The line time comes only from a steady stream, and only a plausible result is taken.
#define LINE_TIME_MIN_FRAME_LINES 100
#define LINE_TIME_EXPOSURE_MARGIN_LINES 8
#define LINE_TIME_MIN_FPS 4
#define LINE_TIME_MIN_FRAMES 30
#define LINE_MICROSECONDS_MIN 30
#define LINE_MICROSECONDS_MAX 200
#define LINE_TIME_SMOOTHING 0.3f

// One sensor line in microseconds, updated from the running stream.
static float g_lineMicroseconds = START_LINE_MICROSECONDS;

// The first failure of a series of writes is the one reported.
static Result keep_first_failure(Result result, Result next)
{
	if (R_FAILED(next) && R_SUCCEEDED(result)) {
		return next;
	}
	return result;
}

// AE_R9 never holds 0: a time below one line there is untested.
static u16 ae_r9_lines(u16 lines)
{
	return lines ? lines : 1;
}

static u16 capped_to_dual_frame(u16 lines)
{
	if (g_camera.dual && lines + SENSOR_EXPOSURE_MARGIN_LINES > g_camera.dualFrameLines) {
		return g_camera.dualFrameLines - SENSOR_EXPOSURE_MARGIN_LINES;
	}
	return lines;
}

static void wait_for_mid_frame(void)
{
	CamPort *viewPort = cam_state_view_port();
	cam_state_wait_mid_frame(viewPort->lastTick, viewPort->periodTicks);
}

static bool contains_device(const u8 *devices, int count, u8 device)
{
	for (int i = 0; i < count; i++) {
		if (devices[i] == device) {
			return true;
		}
	}
	return false;
}

u16 sensor_exposure_even_gain_code(int code)
{
	int clamped = math_clamp_int(code, SENSOR_GAIN_CODE_MIN, SENSOR_GAIN_CODE_MAX);
	return (u16)(clamped & ~1);
}

float sensor_exposure_gain_factor(u16 code)
{
	float evenCode = (float)sensor_exposure_even_gain_code(code);
	float nominalGain = evenCode / SENSOR_GAIN_CODE_MIN;
	float shortfall = 1 - GAIN_SHORTFALL_PER_CODE * (evenCode - SENSOR_GAIN_CODE_MIN);
	return nominalGain * shortfall;
}

// A code of the ISO dial gives the dial's ISO, so the screen and the DNG show ISO 400 and not 399. Every
// other code gives the ISO of its real gain, so the DNG's ISO tag still gives the code back.
int sensor_exposure_gain_iso(u16 code)
{
	u16 evenCode = sensor_exposure_even_gain_code(code);
	for (int i = 0; i < CAM_ISO_COUNT; i++) {
		if (evenCode == CAM_ISO_GAIN_CODES[i]) {
			return CAM_ISO_VALUES[i];
		}
	}
	return (int)(sensor_exposure_gain_factor(evenCode) * ISO_AT_GAIN_X1 + 0.5f);
}

static u16 largest_code_at_most(float gain)
{
	u16 best = SENSOR_GAIN_CODE_MIN;
	for (u16 code = SENSOR_GAIN_CODE_MIN; code <= SENSOR_GAIN_CODE_MAX; code += SENSOR_GAIN_CODE_STEP) {
		if (sensor_exposure_gain_factor(code) <= gain) {
			best = code;
		}
	}
	return best;
}

static u16 smallest_code_at_least(float gain)
{
	for (u16 code = SENSOR_GAIN_CODE_MIN; code <= SENSOR_GAIN_CODE_MAX; code += SENSOR_GAIN_CODE_STEP) {
		if (sensor_exposure_gain_factor(code) >= gain) {
			return code;
		}
	}
	return SENSOR_GAIN_CODE_MAX;
}

static u16 nearest_code(float gain)
{
	u16 best = SENSOR_GAIN_CODE_MIN;
	float bestStops = INFINITY;
	for (u16 code = SENSOR_GAIN_CODE_MIN; code <= SENSOR_GAIN_CODE_MAX; code += SENSOR_GAIN_CODE_STEP) {
		float stops = fabsf(logf(sensor_exposure_gain_factor(code) / gain));
		if (stops < bestStops) {
			bestStops = stops;
			best = code;
		}
	}
	return best;
}

u16 sensor_exposure_gain_code(float gain, GainRounding rounding)
{
	if (rounding == GAIN_ROUND_DOWN) {
		return largest_code_at_most(gain);
	}
	if (rounding == GAIN_ROUND_UP) {
		return smallest_code_at_least(gain);
	}
	return nearest_code(gain);
}

u16 sensor_exposure_fine_time(void)
{
	return g_camera.exposureFineTime ? g_camera.exposureFineTime : SENSOR_FINE_TIME_AUTO;
}

float sensor_exposure_fine_lines(void)
{
	return sensor_exposure_fine_time() / (float)LINE_PIXEL_CLOCKS;
}

float sensor_exposure_seconds(u32 lines)
{
	return (lines + sensor_exposure_fine_lines()) * g_lineMicroseconds / (float)MICROSECONDS_PER_SECOND;
}

float sensor_exposure_line_microseconds(void)
{
	return g_lineMicroseconds;
}

u8 sensor_exposure_view_device(void)
{
	int camera = math_clamp_int(g_camera.settings.camera, 0, CAM_COUNT - 1);
	return SENSOR_I2C_DEVICES[camera];
}

// In dual both outer sensors, OUT1 first.
int sensor_exposure_stream_devices(u8 devices[SENSOR_STREAM_DEVICES_MAX])
{
	if (g_camera.dual) {
		devices[0] = SENSOR_DEVICE_OUT1;
		devices[1] = SENSOR_DEVICE_OUT2;
		return SENSOR_STREAM_DEVICES_MAX;
	}
	devices[0] = sensor_exposure_view_device();
	return 1;
}

// Only with both AE and AWB off does the sensor's MCU leave a direct coarse time alone.
Result sensor_exposure_sequencer_off(u8 device)
{
	u16 mode = SENSOR_SEQ_MODE_AUTO;
	Result result = sensor_i2c_mcu_read(device, SENSOR_MCU_SEQ_MODE, &mode);
	if (R_FAILED(result)) {
		return result;
	}
	return sensor_i2c_mcu_write(device, SENSOR_MCU_SEQ_MODE, mode & ~SENSOR_SEQ_AE_AWB);
}

// The same gain on every colour keeps the raw neutral.
Result sensor_exposure_write_gains(u8 device, u16 gain)
{
	Result result = 0;
	for (u16 reg = SENSOR_REG_GAIN_GLOBAL; reg <= SENSOR_REG_GAIN_LAST && R_SUCCEEDED(result);
	     reg += SENSOR_REG_GAIN_STEP) {
		result = sensor_i2c_write16(device, reg, gain);
	}
	return result;
}

// Without writeCoarse the caller writes the coarse time, so that dual sensors get it back to back.
Result sensor_exposure_write_registers(u8 device, u16 lines, u16 gain, bool writeCoarse)
{
	Result result = sensor_i2c_mcu_write(device, MCU_AE_R9, ae_r9_lines(lines));
	if (R_SUCCEEDED(result) && writeCoarse) {
		result = sensor_i2c_write16(device, SENSOR_REG_COARSE_TIME, lines);
	}
	if (R_SUCCEEDED(result)) {
		result = sensor_i2c_write16(device, SENSOR_REG_FINE_TIME, sensor_exposure_fine_time());
	}
	if (R_SUCCEEDED(result)) {
		result = sensor_exposure_write_gains(device, gain);
	}
	return result;
}

Result sensor_exposure_write_device(u8 device, u16 lines, u16 gain)
{
	Result result = sensor_exposure_sequencer_off(device);
	svcSleepThread(SEQUENCER_STOP_WAIT_NS);
	if (R_FAILED(result)) {
		return result;
	}
	return sensor_exposure_write_registers(device, lines, gain, true);
}

// The sensor's own AE moves the time and the gains, its AWB the colour gains. A failed read counts as ours.
static bool exposure_is_ours(u8 device)
{
	u16 value = 0;
	if (R_SUCCEEDED(sensor_i2c_read16(device, SENSOR_REG_COARSE_TIME, &value)) &&
	    value != g_camera.exposureLines) {
		return false;
	}
	for (u16 reg = SENSOR_REG_GAIN_GREEN; reg <= SENSOR_REG_GAIN_LAST; reg += SENSOR_REG_GAIN_STEP) {
		if (R_SUCCEEDED(sensor_i2c_read16(device, reg, &value)) && (value & ~1) != g_camera.exposureGain) {
			return false;
		}
	}
	return true;
}

// The coarse time on several sensors back to back. In dual the new time never stretches the frame, but the
// one it replaces may have. Ending a stretch changes the frame's length, so that write goes in mid-frame:
// both sensors take it on the same frame and stay frame-locked.
static Result write_coarse_time(const u8 *devices, int deviceCount, u16 lines)
{
	if (deviceCount > 1 && g_camera.dual) {
		u16 frameLines = cam_state_frame_lines();
		if (g_camera.exposureLines + SENSOR_EXPOSURE_MARGIN_LINES > frameLines) {
			wait_for_mid_frame();
		}
	}
	Result firstFailure = 0;
	for (int i = 0; i < deviceCount; i++) {
		firstFailure = keep_first_failure(firstFailure,
		                                  sensor_i2c_write16(devices[i], SENSOR_REG_COARSE_TIME, lines));
	}
	return firstFailure;
}

static Result write_frame_lengths(const u8 *devices, int deviceCount)
{
	Result firstFailure = 0;
	for (int i = 0; i < deviceCount; i++) {
		Result result = sensor_i2c_write16(devices[i], SENSOR_REG_FRAME_LENGTH, cam_state_frame_lines());
		firstFailure = keep_first_failure(firstFailure, result);
	}
	return firstFailure;
}

void sensor_exposure_park_devices(const u8 *devices, int deviceCount, u16 lines)
{
	if (lines <= SENSOR_FULL_FRAME_LINES) {
		return;
	}
	write_coarse_time(devices, deviceCount, SENSOR_PARK_LINES);
	g_camera.parkedLines = lines;
}

void sensor_exposure_park(void)
{
	if (!g_camera.running || !g_camera.rawActive) {
		return;
	}
	u8 devices[SENSOR_STREAM_DEVICES_MAX];
	int deviceCount = sensor_exposure_stream_devices(devices);
	sensor_exposure_park_devices(devices, deviceCount, g_camera.exposureLines);
}

bool sensor_exposure_held_lines(u16 *lines)
{
	if (!g_camera.rawActive || g_camera.settings.exposureMode == CAM_EXPOSURE_AUTO ||
	    !g_camera.exposureGain) {
		return false;
	}
	*lines = g_camera.heldLines;
	return true;
}

// Times between L lines + FINE_TIME_MAX and L + 1 lines + FINE_TIME_MIN cannot be set: they take the nearer
// end.
bool sensor_exposure_manual_time(const CamSettings *settings, u16 *lines, u16 *fine)
{
	*lines = 0;
	*fine = SENSOR_FINE_TIME_AUTO;
	if (settings->exposureMode == CAM_EXPOSURE_AUTO) {
		return false;
	}
	int shutter = math_clamp_int(settings->shutter, 0, CAM_SHUTTER_COUNT - 1);
	float microseconds = CAM_SHUTTER_SECONDS[shutter] * (float)MICROSECONDS_PER_SECOND;
	float exactLines = microseconds / g_lineMicroseconds;
	float pixelClocks = exactLines * LINE_PIXEL_CLOCKS;
	float wholeLines = floorf((pixelClocks - FINE_TIME_MIN) / LINE_PIXEL_CLOCKS);
	if (wholeLines < 0) {
		wholeLines = 0;
	}
	if (wholeLines > UINT16_MAX) {
		wholeLines = UINT16_MAX;
	}
	float remainder = pixelClocks - wholeLines * LINE_PIXEL_CLOCKS;
	if (remainder > FINE_TIME_MAX) {
		float aboveMax = remainder - FINE_TIME_MAX;
		float belowNextMin = LINE_PIXEL_CLOCKS + FINE_TIME_MIN - remainder;
		if (aboveMax > belowNextMin && wholeLines < UINT16_MAX) {
			wholeLines += 1;
			remainder = FINE_TIME_MIN;
		} else {
			remainder = FINE_TIME_MAX;
		}
	}
	if (remainder < FINE_TIME_MIN) {
		remainder = FINE_TIME_MIN;
	}
	*lines = (u16)wholeLines;
	*fine = (u16)(remainder + 0.5f);
	return true;
}

// The time the view sensor has right now, as read.
static u16 coarse_lines(u8 viewDevice)
{
	u16 lines = 0;
	sensor_i2c_read16(viewDevice, SENSOR_REG_COARSE_TIME, &lines);
	if (lines < 1) {
		lines = 1;
	}
	return lines;
}

// Auto starts from the time the view sensor is using, or from the one a park cut short.
static u16 current_lines(u8 viewDevice)
{
	u16 lines = coarse_lines(viewDevice);
	if (g_camera.parkedLines && lines == SENSOR_PARK_LINES) {
		lines = g_camera.parkedLines;
	}
	return lines;
}

// Manual uses the ISO's gain. Auto and shutter priority start from equal gains at the current green gain.
static u16 starting_gain_code(const CamSettings *settings, u8 viewDevice)
{
	if (settings->exposureMode == CAM_EXPOSURE_MANUAL) {
		return CAM_ISO_GAIN_CODES[(int)settings->iso];
	}
	u16 code = SENSOR_GAIN_CODE_MIN;
	sensor_i2c_read16(viewDevice, SENSOR_REG_GAIN_GREEN, &code);
	return sensor_exposure_even_gain_code(code);
}

void sensor_exposure_metered(const CamSettings *settings, u16 *lines, u16 *gain)
{
	u8 viewDevice = sensor_exposure_view_device();
	u16 fine = SENSOR_FINE_TIME_AUTO;
	if (sensor_exposure_manual_time(settings, lines, &fine)) {
		g_camera.exposureFineTime = fine;
	} else {
		*lines = coarse_lines(viewDevice); // as read: a burst does not take back a time a park cut short
	}
	*gain = starting_gain_code(settings, viewDevice);
}

static Result switch_sequencers_off(const u8 *devices, int deviceCount, bool *switchedOff)
{
	Result firstFailure = 0;
	for (int i = 0; i < deviceCount; i++) {
		Result result = sensor_exposure_sequencer_off(devices[i]);
		switchedOff[i] = R_SUCCEEDED(result);
		firstFailure = keep_first_failure(firstFailure, result);
	}
	return firstFailure;
}

// A frame lasts at least as long as its exposure. In dual the frame is fixed.
static u32 stall_limit_ms(const CamSettings *settings)
{
	float frameSeconds = settings->exposureMode == CAM_EXPOSURE_AUTO
	                             ? AUTO_LONGEST_SECONDS
	                             : CAM_SHUTTER_SECONDS[(int)settings->shutter];
	if (g_camera.dual) {
		frameSeconds = g_camera.dualFrameLines * g_lineMicroseconds / (float)MICROSECONDS_PER_SECOND;
	}
	return cam_state_stall_limit_ms(frameSeconds);
}

Result sensor_exposure_apply(const CamSettings *settings)
{
	if (!g_camera.rawActive) {
		return 0;
	}
	u8 viewDevice = sensor_exposure_view_device();
	u8 devices[SENSOR_STREAM_DEVICES_MAX];
	int deviceCount = sensor_exposure_stream_devices(devices);
	u16 lines = 0;
	u16 fine = SENSOR_FINE_TIME_AUTO;
	bool manualTime = sensor_exposure_manual_time(settings, &lines, &fine);
	if (!manualTime) {
		lines = current_lines(viewDevice);
	}
	g_camera.parkedLines = 0;
	bool switchedOff[SENSOR_STREAM_DEVICES_MAX] = {false};
	Result firstFailure = switch_sequencers_off(devices, deviceCount, switchedOff);
	svcSleepThread(SEQUENCER_STOP_WAIT_NS);
	lines = capped_to_dual_frame(lines);
	g_camera.exposureFineTime = fine;
	u16 gain = starting_gain_code(settings, viewDevice);
	u8 written[SENSOR_STREAM_DEVICES_MAX];
	int writtenCount = 0;
	for (int i = 0; i < deviceCount; i++) {
		if (!switchedOff[i]) {
			continue;
		}
		Result result = sensor_exposure_write_registers(devices[i], lines, gain, deviceCount == 1);
		firstFailure = keep_first_failure(firstFailure, result);
		written[writtenCount] = devices[i];
		writtenCount++;
	}
	if (deviceCount > 1) {
		firstFailure = keep_first_failure(firstFailure, write_coarse_time(written, writtenCount, lines));
	}
	// The module's AE may have changed the dual frame length. Nothing else writes it until the next start.
	if (g_camera.dual) {
		firstFailure = keep_first_failure(firstFailure, write_frame_lengths(written, writtenCount));
	}
	g_camera.exposureLines = lines;
	g_camera.heldLines = lines;
	g_camera.exposureGain = gain;
	// 0 makes the guard write AE_R9.
	g_camera.aeR9Lines = R_SUCCEEDED(firstFailure) ? ae_r9_lines(lines) : 0;
	g_camera.exposureWriteFrame = cam_state_view_port()->framesDone;
	g_camera.stallMs = stall_limit_ms(settings);
	return firstFailure;
}

// Direct registers only: another thread may be in the middle of a two-step MCU access. AE_R9 follows
// through the guard.
Result sensor_exposure_set_lines(u16 lines)
{
	if (!g_camera.rawActive) {
		return CAM_ERROR_NOT_RAW;
	}
	lines = capped_to_dual_frame(lines);
	if (g_camera.hdrOn) {
		g_camera.exposureLines = lines;
		return 0;
	}
	u8 devices[SENSOR_STREAM_DEVICES_MAX];
	int deviceCount = sensor_exposure_stream_devices(devices);
	Result result = write_coarse_time(devices, deviceCount, lines);
	if (lines != g_camera.exposureLines) {
		g_camera.exposureLines = lines;
		g_camera.exposureWriteFrame = cam_state_view_port()->framesDone;
	}
	return result;
}

Result sensor_exposure_set_gain(u16 code)
{
	if (!g_camera.rawActive) {
		return CAM_ERROR_NOT_RAW;
	}
	// Even codes only, so the read-back matches what the sensor uses.
	code &= ~1;
	if (g_camera.hdrOn) {
		g_camera.exposureGain = code;
		return 0;
	}
	u8 devices[SENSOR_STREAM_DEVICES_MAX];
	int deviceCount = sensor_exposure_stream_devices(devices);
	Result firstFailure = 0;
	for (int i = 0; i < deviceCount; i++) {
		firstFailure = keep_first_failure(firstFailure, sensor_exposure_write_gains(devices[i], code));
	}
	if (code != g_camera.exposureGain) {
		g_camera.exposureGain = code;
		g_camera.exposureWriteFrame = cam_state_view_port()->framesDone;
	}
	return firstFailure;
}

static void mark_foreign(SensorExposure *exposure)
{
	exposure->foreign = true;
	sensor_exposure_ask_guard();
}

static void check_fine_time(u16 fine, SensorExposure *exposure)
{
	if (!g_camera.exposureGain || fine == sensor_exposure_fine_time()) {
		return;
	}
	mark_foreign(exposure);
}

static void check_time_and_gains(u8 device, SensorExposure *exposure)
{
	if (!g_camera.exposureGain || g_camera.hdrOn || exposure_is_ours(device)) {
		return;
	}
	mark_foreign(exposure);
}

// The camera module turns the sensors' own AE and AWB back on after a camera switch or a stream restart. That
// shows as a time, a gain or a fine time that is not the one written. Only single register reads here: the
// guard on the camera thread does the rest. Returns the view sensor's fine time, 0 when unread.
static u16 check_streaming_sensors(u8 viewDevice, SensorExposure *exposure)
{
	u8 devices[SENSOR_STREAM_DEVICES_MAX];
	int deviceCount = sensor_exposure_stream_devices(devices);
	u16 viewFine = 0;
	for (int i = 0; i < deviceCount; i++) {
		u8 device = devices[i];
		u16 fine = 0;
		if (R_SUCCEEDED(sensor_i2c_read16(device, SENSOR_REG_FINE_TIME, &fine))) {
			if (device == viewDevice) {
				viewFine = fine;
			}
			check_fine_time(fine, exposure);
		}
		check_time_and_gains(device, exposure);
	}
	return viewFine;
}

// An app without a camera thread never calls sensor_exposure_guard: a request the previous read left pending
// runs here.
static void run_guard_left_pending(void)
{
	if (sensor_exposure_guard_wanted() && !g_camera.guardByCaller && g_camera.guardSeenPending) {
		sensor_exposure_run_guard();
	}
	g_camera.guardSeenPending = sensor_exposure_guard_wanted();
}

// Frames last frame-length lines while the exposure is shorter, so the frame rate gives the line time. Only
// frames that all had the same length count: the time and the frame length unchanged for CAM_LINE_HOLD_MS.
static void update_line_time(u8 viewDevice, u16 lines)
{
	static u16 lastLines;
	static u16 lastFrameLines;
	static u64 steadySince;
	u16 frameLines = 0;
	CamPort *viewPort = cam_state_view_port();
	u64 now = osGetTime();
	Result frameRead = sensor_i2c_read16(viewDevice, SENSOR_REG_FRAME_LENGTH, &frameLines);
	if (lines != lastLines || frameLines != lastFrameLines) {
		lastLines = lines;
		lastFrameLines = frameLines;
		steadySince = now;
	}
	bool frameKnown = R_SUCCEEDED(frameRead) && frameLines > LINE_TIME_MIN_FRAME_LINES;
	if (!frameKnown) {
		return;
	}
	bool exposureInsideFrame = lines + LINE_TIME_EXPOSURE_MARGIN_LINES < frameLines;
	if (!exposureInsideFrame) {
		return;
	}
	bool rateKnown = viewPort->fps > LINE_TIME_MIN_FPS && viewPort->frames > LINE_TIME_MIN_FRAMES;
	if (!rateKnown) {
		return;
	}
	bool frameSteady = now >= g_camera.lineHoldUntil && now - steadySince >= CAM_LINE_HOLD_MS;
	if (!frameSteady) {
		return;
	}
	float lineMicroseconds = (float)MICROSECONDS_PER_SECOND / (viewPort->fps * frameLines);
	if (lineMicroseconds > LINE_MICROSECONDS_MIN && lineMicroseconds < LINE_MICROSECONDS_MAX) {
		g_lineMicroseconds += (lineMicroseconds - g_lineMicroseconds) * LINE_TIME_SMOOTHING;
	}
}

Result sensor_exposure_read(SensorExposure *exposure)
{
	memset(exposure, 0, sizeof *exposure);
	if (!g_camera.rawActive) {
		return CAM_ERROR_NOT_RAW;
	}
	u8 viewDevice = sensor_exposure_view_device();
	u16 viewFine = check_streaming_sensors(viewDevice, exposure);
	// AE_R9 follows a new time here, at most once per read, not on every step of the exposure loop.
	if (g_camera.exposureGain && !g_camera.hdrOn &&
	    g_camera.aeR9Lines != ae_r9_lines(g_camera.exposureLines)) {
		sensor_exposure_ask_guard();
	}
	run_guard_left_pending();
	Result result = sensor_i2c_read16(viewDevice, SENSOR_REG_COARSE_TIME, &exposure->lines);
	if (R_SUCCEEDED(result)) {
		result = sensor_i2c_read16(viewDevice, SENSOR_REG_GAIN_GREEN, &exposure->gain);
	}
	if (R_FAILED(result)) {
		return result;
	}
	update_line_time(viewDevice, exposure->lines);
	u16 fine = viewFine ? viewFine : sensor_exposure_fine_time();
	float fineLines = fine / (float)LINE_PIXEL_CLOCKS;
	exposure->seconds = (exposure->lines + fineLines) * g_lineMicroseconds / (float)MICROSECONDS_PER_SECOND;
	exposure->iso = sensor_exposure_gain_iso(exposure->gain);
	exposure->valid = true;
	return 0;
}

// The sensors whose AE or AWB the module switched back on, or whose time or gains are not Pro Cam's.
static int find_sensors_to_fix(const u8 *devices, int deviceCount, u8 *toFix, bool *sequencerOn)
{
	int fixCount = 0;
	for (int i = 0; i < deviceCount; i++) {
		u16 mode = 0;
		if (R_FAILED(sensor_i2c_mcu_read(devices[i], SENSOR_MCU_SEQ_MODE, &mode))) {
			continue;
		}
		bool sequencerRuns = (mode & SENSOR_SEQ_AE_AWB) != 0;
		if (!sequencerRuns && (!g_camera.exposureGain || exposure_is_ours(devices[i]))) {
			continue;
		}
		sequencerOn[fixCount] = sequencerRuns;
		toFix[fixCount] = devices[i];
		fixCount++;
	}
	return fixCount;
}

// One register and no wait: the module's AE is not involved.
static void restore_fine_time(u8 device)
{
	u16 fine = 0;
	if (R_FAILED(sensor_i2c_read16(device, SENSOR_REG_FINE_TIME, &fine)) ||
	    fine == sensor_exposure_fine_time()) {
		return;
	}
	sensor_i2c_write16(device, SENSOR_REG_FINE_TIME, sensor_exposure_fine_time());
	// A burst lets the change settle, as after any exposure write.
	g_camera.exposureWriteFrame = cam_state_view_port()->framesDone;
}

// The sensors that are not fixed in full still get AE_R9 in step with the time and their fine time back.
static void restore_r9_and_fine_time(const u8 *devices, int deviceCount, const u8 *toFix, int fixCount)
{
	u16 r9Lines = ae_r9_lines(g_camera.exposureLines);
	bool r9Written = true;
	for (int i = 0; i < deviceCount; i++) {
		u8 device = devices[i];
		if (contains_device(toFix, fixCount, device)) {
			continue;
		}
		if (g_camera.aeR9Lines != r9Lines && R_FAILED(sensor_i2c_mcu_write(device, MCU_AE_R9, r9Lines))) {
			r9Written = false;
		}
		restore_fine_time(device);
	}
	if (r9Written) {
		g_camera.aeR9Lines = r9Lines;
	}
}

// One camera: the module's AE may have changed the frame length, which nothing else writes until the next
// start. In the fast view the module may also have reprogrammed the row skipping. Both are read first, so
// only a change is written.
static Result restore_single_frame(u8 device)
{
	u16 wanted = g_camera.fastViewOn ? (u16)(g_camera.frameLinesNormal - FAST_VIEW_FRAME_CUT)
	                                 : g_camera.frameLinesNormal;
	u16 frameLines = 0;
	u16 readMode = 0;
	sensor_i2c_read16(device, SENSOR_REG_FRAME_LENGTH, &frameLines);
	sensor_i2c_read16(device, SENSOR_REG_READ_MODE, &readMode);
	u16 rowStep = readMode & SENSOR_READ_MODE_Y_ODD_INC_MASK;
	bool rowSkippingLost = g_camera.fastViewOn && rowStep != SENSOR_READ_ROWS_SKIPPED;
	if (frameLines == wanted && !rowSkippingLost) {
		return 0;
	}
	if (rowSkippingLost) {
		u16 skippingMode = (readMode & ~SENSOR_READ_MODE_Y_ODD_INC_MASK) | SENSOR_READ_ROWS_SKIPPED;
		sensor_i2c_write16(device, SENSOR_REG_READ_MODE, skippingMode);
	}
	return sensor_i2c_write16(device, SENSOR_REG_FRAME_LENGTH, wanted);
}

// Dual: the module's AE may have stretched the frames. The time and the frame length go to the sensors back
// to back, mid-frame when both get them, so both take them on the same frame.
static Result restore_dual_frames(const u8 *devices, int deviceCount)
{
	if (deviceCount > 1) {
		wait_for_mid_frame();
	}
	Result firstFailure = 0;
	for (int i = 0; i < deviceCount; i++) {
		Result result = sensor_i2c_write16(devices[i], SENSOR_REG_COARSE_TIME, g_camera.exposureLines);
		firstFailure = keep_first_failure(firstFailure, result);
	}
	return keep_first_failure(firstFailure, write_frame_lengths(devices, deviceCount));
}

// AE and AWB go off again where the module switched them on, with one wait for all the sensors. Where they
// are already off only the exposure goes back.
static Result rewrite_exposure(const u8 *toFix, const bool *sequencerOn, int fixCount)
{
	Result firstFailure = 0;
	bool switchedOff[SENSOR_STREAM_DEVICES_MAX] = {false};
	bool sequencerSwitched = false;
	for (int i = 0; i < fixCount; i++) {
		if (!sequencerOn[i]) {
			switchedOff[i] = true;
			continue;
		}
		Result result = sensor_exposure_sequencer_off(toFix[i]);
		switchedOff[i] = R_SUCCEEDED(result);
		sequencerSwitched = true;
		firstFailure = keep_first_failure(firstFailure, result);
	}
	if (sequencerSwitched) {
		svcSleepThread(SEQUENCER_STOP_WAIT_NS);
	}
	u8 written[SENSOR_STREAM_DEVICES_MAX];
	int writtenCount = 0;
	for (int i = 0; i < fixCount; i++) {
		if (!switchedOff[i]) {
			continue;
		}
		Result result = sensor_exposure_write_registers(toFix[i], g_camera.exposureLines,
		                                                g_camera.exposureGain, !g_camera.dual);
		firstFailure = keep_first_failure(firstFailure, result);
		written[writtenCount] = toFix[i];
		writtenCount++;
	}
	if (!g_camera.dual && writtenCount && g_camera.frameLinesNormal) {
		firstFailure = keep_first_failure(firstFailure, restore_single_frame(written[0]));
	}
	if (g_camera.dual && writtenCount) {
		firstFailure = keep_first_failure(firstFailure, restore_dual_frames(written, writtenCount));
	}
	g_camera.exposureWriteFrame = cam_state_view_port()->framesDone;
	return firstFailure;
}

Result sensor_exposure_run_guard(void)
{
	// Cleared first, so that a restart from now on asks again.
	g_camera.guardRequested = false;
	g_camera.guardSeenPending = false;
	g_camera.guardDueMs = 0;
	if (!g_camera.running || !g_camera.rawActive) {
		return 0;
	}
	u64 now = osGetTime();
	u8 devices[SENSOR_STREAM_DEVICES_MAX];
	int deviceCount = sensor_exposure_stream_devices(devices);
	u8 toFix[SENSOR_STREAM_DEVICES_MAX];
	bool sequencerOn[SENSOR_STREAM_DEVICES_MAX] = {false};
	int fixCount = find_sensors_to_fix(devices, deviceCount, toFix, sequencerOn);
	// Within a (re)start's window the module may still switch AE and AWB on: look again once it has passed.
	if (now < g_camera.unverifiedUntil) {
		g_camera.guardRequested = true;
		g_camera.guardDueMs = g_camera.unverifiedUntil;
	}
	if (g_camera.exposureGain) {
		restore_r9_and_fine_time(devices, deviceCount, toFix, fixCount);
	}
	if (!fixCount) {
		return 0;
	}
	// Nothing written since the start: the whole setup.
	if (!g_camera.exposureGain) {
		return sensor_exposure_apply(&g_camera.settings);
	}
	return rewrite_exposure(toFix, sequencerOn, fixCount);
}

// Within a (re)start's window the module makes MCU accesses of its own, which the guard's two-step ones could
// interleave with. So the guard is due once the window has passed, or at once outside one.
void sensor_exposure_ask_guard(void)
{
	g_camera.guardDueMs = g_camera.unverifiedUntil;
	g_camera.guardRequested = true;
}

bool sensor_exposure_guard_wanted(void)
{
	if (!g_camera.running || !g_camera.rawActive) {
		return false;
	}
	return osGetTime() >= g_camera.guardDueMs && g_camera.guardRequested;
}

bool sensor_exposure_unverified(void)
{
	return g_camera.running && g_camera.rawActive && osGetTime() < g_camera.unverifiedUntil;
}

// Never inside dual_burst_take.
Result sensor_exposure_guard(void)
{
	// From now on sensor_exposure_read leaves the guard to the caller.
	g_camera.guardByCaller = true;
	return sensor_exposure_run_guard();
}
