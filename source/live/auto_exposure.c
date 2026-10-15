#include "auto_exposure.h"
#include <3ds.h>
#include <math.h>
#include "cam_settings.h"
#include "dual_stream.h"
#include "metering.h"
#include "raw_view.h"
#include "sensor_exposure.h"
#include "time_units.h"

// The exposure is read back faster just after a start: the camera module may switch a sensor's own AE and
// AWB back on then.
#define READBACK_MS 500
#define READBACK_AFTER_START_MS 100
#define AUTO_EXPOSURE_STEP_MS 200

// Each EV step is a full stop around mid grey, 18 % of the raw range.
#define MID_GREY_SHARE 0.18f
// A floor for the metered level, so a black frame does not ask for an endless exposure.
#define METER_FLOOR 0.5f
// One step moves the exposure two stops at most.
#define STEP_RATIO_MIN 0.25f
#define STEP_RATIO_MAX 4.0f
// The step ratio is the square root of the error, so this band is about 1/8 stop of error.
#define DEAD_BAND_LOW 0.957f
#define DEAD_BAND_HIGH 1.045f
// The longest time auto uses before it raises the gain: 1/15 s.
#define HAND_HELD_MICROSECONDS 66667.0f
// The gain steps down only when the next code down leaves 10 % of the hand-held time to spare.
#define GAIN_DOWN_HEADROOM 0.9f
// A change shows only in frames exposed after the sensor took it.
#define FRAMES_TO_SETTLE 3

SensorExposure g_exposureNow;
static u64 g_exposureReadAt;
static u64 g_autoExposureStepAt;

float auto_exposure_max_lines(void)
{
	return AUTO_EXPOSURE_MAX_SECONDS * (float)MICROSECONDS_PER_SECOND / sensor_exposure_line_microseconds();
}

static float hand_held_lines(void)
{
	return HAND_HELD_MICROSECONDS / sensor_exposure_line_microseconds();
}

// Half the way to the metered target, in stops, so the loop settles without hunting.
static float exposure_step_ratio(const CamSettings *settings)
{
	float meterGreen = metering_green();
	float meter = meterGreen < METER_FLOOR ? METER_FLOOR : meterGreen;
	float midGrey = MID_GREY_SHARE * (RAW10_WHITE - RAW10_BLACK);
	float target = midGrey * powf(2.0f, (float)settings->exposure);
	float ratio = sqrtf(target / meter);
	if (ratio < STEP_RATIO_MIN) {
		ratio = STEP_RATIO_MIN;
	} else if (ratio > STEP_RATIO_MAX) {
		ratio = STEP_RATIO_MAX;
	}
	return ratio;
}

// Auto: the smallest gain code that keeps the time within the hand-held limit. A gain step (8 to 10 is 25 %)
// is coarser than the dead band, so the gain steps down only with headroom and the time makes up the rest.
static int auto_gain_code(float exposure, float handLines)
{
	int gain = sensor_exposure_even_gain_code(g_exposureNow.gain);
	if (exposure / sensor_exposure_gain_factor(gain) > handLines) {
		gain = sensor_exposure_gain_code(exposure / handLines, GAIN_ROUND_UP);
	}
	float downLimit = handLines * GAIN_DOWN_HEADROOM;
	while (gain > SENSOR_GAIN_CODE_MIN &&
	       exposure / sensor_exposure_gain_factor(gain - SENSOR_GAIN_CODE_STEP) <= downLimit) {
		gain -= SENSOR_GAIN_CODE_STEP;
	}
	return gain;
}

// Auto: the whole lines that give the exposure at this gain, rounded, within 1 .. maxLines.
static int auto_lines(float exposure, int gain, float fineLines, float maxLines)
{
	int lines = (int)(exposure / sensor_exposure_gain_factor(gain) - fineLines + 0.5f);
	if (lines > maxLines) {
		lines = (int)maxLines;
	}
	if (lines < 1) {
		lines = 1;
	}
	return lines;
}

// Pro Cam's own exposure loop for raw, with the sensor's AE off. Auto raises the time first at x1 gain up to
// the hand-held limit, then the gain, then the time up to the maximum. Shutter priority moves only the gain.
static void auto_exposure_step(const CamSettings *settings)
{
	static u32 settleUntilFrame;
	if (!g_exposureNow.valid || metering_green() < 0 || metering_frames() < settleUntilFrame) {
		return;
	}
	float ratio = exposure_step_ratio(settings);
	if (ratio > DEAD_BAND_LOW && ratio < DEAD_BAND_HIGH) {
		return;
	}
	// The exposure as real time times real gain: the whole lines with the fine part, times the gain factor.
	float fineLines = sensor_exposure_fine_lines();
	float exposure =
	        (g_exposureNow.lines + fineLines) * sensor_exposure_gain_factor(g_exposureNow.gain) * ratio;
	// The dual stream runs at a fixed frame length, and one stretched frame shifts the two sensors' timing
	// for good. So there the time stays within one frame and the gain makes up the rest.
	float maxLines = dual_stream_active() ? (float)DUAL_MAX_EXPOSURE_LINES : auto_exposure_max_lines();
	float handLines = hand_held_lines() < maxLines ? hand_held_lines() : maxLines;
	int lines;
	int gain;
	if (settings->exposureMode == CAM_EXPOSURE_SHUTTER_PRIORITY) {
		lines = g_exposureNow.lines;
		gain = sensor_exposure_gain_code(exposure / (lines + fineLines), GAIN_ROUND_NEAREST);
	} else {
		gain = auto_gain_code(exposure, handLines);
		lines = auto_lines(exposure, gain, fineLines, maxLines);
	}
	gain = sensor_exposure_even_gain_code(gain);
	if (lines == (int)g_exposureNow.lines && gain == (int)g_exposureNow.gain) {
		return;
	}
	if (lines != (int)g_exposureNow.lines) {
		sensor_exposure_set_lines((u16)lines);
		g_exposureNow.lines = lines;
	}
	if (gain != (int)g_exposureNow.gain) {
		sensor_exposure_set_gain((u16)gain);
		g_exposureNow.gain = gain;
	}
	settleUntilFrame = metering_frames() + FRAMES_TO_SETTLE;
	g_exposureNow.seconds = sensor_exposure_seconds(g_exposureNow.lines);
	g_exposureNow.iso = sensor_exposure_gain_iso(g_exposureNow.gain);
}

void auto_exposure_poll(const CamSettings *settings, u64 nowMs)
{
	u64 sinceReadMs = nowMs - g_exposureReadAt;
	u64 readbackMs = sensor_exposure_unverified() ? READBACK_AFTER_START_MS : READBACK_MS;
	if (sinceReadMs > readbackMs) {
		g_exposureReadAt = nowMs;
		sensor_exposure_read(&g_exposureNow);
		// Manual and shutter priority keep the whole lines chosen with the settings. The line time read back
		// drifts a little, so a second split could flip a line against the fine part.
		u16 heldLines;
		if (sensor_exposure_held_lines(&heldLines) && g_exposureNow.valid &&
		    g_exposureNow.lines != heldLines) {
			sensor_exposure_set_lines(heldLines);
			g_exposureNow.lines = heldLines;
		}
	}
	// The loop rests for a manual time and while the exposure is locked.
	bool loopRuns = settings->exposureMode != CAM_EXPOSURE_MANUAL && settings->autoExposure;
	if (loopRuns && nowMs - g_autoExposureStepAt > AUTO_EXPOSURE_STEP_MS) {
		g_autoExposureStepAt = nowMs;
		auto_exposure_step(settings);
	}
}

void auto_exposure_clear(void)
{
	g_exposureNow.valid = false;
}
