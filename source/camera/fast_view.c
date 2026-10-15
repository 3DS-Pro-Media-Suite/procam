#include "fast_view.h"
#include <3ds.h>
#include "cam_config.h"
#include "cam_settings.h"
#include "cam_state.h"
#include "capture_thread.h"
#include "dual_stream.h"
#include "sensor_exposure.h"
#include "sensor_i2c.h"
#include "time_units.h"

// A single stream's full frame length outside this range is a stretched frame, not the module's own 30 fps
// frame that the fast mode's frame is cut from.
#define NORMAL_FRAME_LINES_MIN 500
#define NORMAL_FRAME_LINES_MAX 600

#define SWITCH_WAIT_MAX_MS 1600 // the longest wait for the old mode's frames to pass
#define CPU_TICKS_PER_MICROSECOND (CPU_TICKS_PER_MSEC / (double)MICROSECONDS_PER_MILLISECOND)

// The fast live view's frame is derived from the full frame length, so a stretched one would make it crawl.
void fast_view_fix_frame_length(void)
{
	u8 device = sensor_exposure_view_device();
	u16 frameLines = 0;
	Result readResult = sensor_i2c_read16(device, SENSOR_REG_FRAME_LENGTH, &frameLines);
	bool plausible = R_SUCCEEDED(readResult) && frameLines >= NORMAL_FRAME_LINES_MIN &&
	                 frameLines <= NORMAL_FRAME_LINES_MAX && frameLines != DUAL_FRAME_LINES;
	g_camera.frameLinesNormal = plausible ? frameLines : SENSOR_FULL_FRAME_LINES;
	if (!plausible) {
		sensor_i2c_write16(device, SENSOR_REG_FRAME_LENGTH, g_camera.frameLinesNormal);
	}
}

// The fast mode reads every other row pair and shortens the frame by as much, so 640 x 240 comes at twice the
// frame rate on the same pixel clock. The Bayer order stays RGGB. An exposure longer than the short frame
// stretches it.
Result fast_view_set_sensor(bool on)
{
	if (on == g_camera.fastViewOn) {
		return 0;
	}
	u8 device = sensor_exposure_view_device();
	Result result = 0;
	if (on) {
		u16 readMode = 0;
		result = sensor_i2c_read16(device, SENSOR_REG_READ_MODE, &readMode);
		if (R_FAILED(result)) {
			return result;
		}
		u16 readModeOtherBits = readMode & ~SENSOR_READ_MODE_Y_ODD_INC_MASK;
		g_camera.readModeNormal = readModeOtherBits | SENSOR_READ_ROWS_ALL;
		if (!g_camera.frameLinesNormal) {
			fast_view_fix_frame_length(); // the sensor is at full frames here
		}
		result = sensor_i2c_write16(device, SENSOR_REG_READ_MODE,
		                            readModeOtherBits | SENSOR_READ_ROWS_SKIPPED);
		if (R_SUCCEEDED(result)) {
			u16 fastFrameLines = g_camera.frameLinesNormal - FAST_VIEW_FRAME_CUT;
			result = sensor_i2c_write16(device, SENSOR_REG_FRAME_LENGTH, fastFrameLines);
		}
	} else {
		result = sensor_i2c_write16(device, SENSOR_REG_READ_MODE, g_camera.readModeNormal);
		if (R_SUCCEEDED(result)) {
			result = sensor_i2c_write16(device, SENSOR_REG_FRAME_LENGTH, g_camera.frameLinesNormal);
		}
	}
	g_camera.fastViewOn = on && R_SUCCEEDED(result);
	// The line time comes from the frame rate and the frame length. Right after a switch the rate still
	// covers the other mode's frames, so the rate starts afresh and the line time holds meanwhile.
	CamPort *viewPort = cam_state_view_port();
	viewPort->fps = 0;
	viewPort->fpsFrames = 0;
	viewPort->fpsStart = osGetTime();
	g_camera.lineHoldUntil = viewPort->fpsStart + CAM_LINE_HOLD_MS;
	return result;
}

void fast_view_update_frame_size(void)
{
	g_camera.frameWidth = CAM_FRAME_WIDTH;
	g_camera.frameHeight = g_camera.fastViewOn ? CAM_FRAME_HEIGHT / 2 : CAM_FRAME_HEIGHT;
}

void fast_view_set_wanted(bool on)
{
	g_camera.fastViewWanted = on;
}

bool fast_view_active(void)
{
	return g_camera.fastViewOn;
}

// The frame as the sensor runs it now, in lines: an exposure longer than the frame stretches it.
static u32 running_frame_lines(void)
{
	u32 frameLines = SENSOR_FULL_FRAME_LINES;
	if (g_camera.frameLinesNormal) {
		frameLines = g_camera.frameLinesNormal;
		if (g_camera.fastViewOn) {
			frameLines -= FAST_VIEW_FRAME_CUT;
		}
	}
	u32 exposureFrameLines = (u32)g_camera.exposureLines + SENSOR_EXPOSURE_MARGIN_LINES;
	return exposureFrameLines > frameLines ? exposureFrameLines : frameLines;
}

// The sensor takes the new read mode and frame length at a frame boundary, and the port catches whatever
// frame starts after its StartCapture. So the registers are written mid-frame, the rest of that frame and
// most of the next one pass, and only then the port starts. The wait follows the frame as the sensor runs
// it: the port's own period is smoothed over frames and lags behind a switch.
Result fast_view_switch(bool on)
{
	if (!g_camera.running || !g_camera.rawActive || g_camera.dual) {
		return 0;
	}
	if (on == g_camera.fastViewOn) {
		return 0;
	}
	Result err = 0;
	CamPort *viewPort = cam_state_view_port();
	u64 lastTick = viewPort->lastTick;
	u32 frameLines = running_frame_lines();
	u64 period = (u64)(frameLines * sensor_exposure_line_microseconds() * CPU_TICKS_PER_MICROSECOND);
	capture_thread_pause();
	CAMU_StopCapture(g_camera.livePort);
	cam_state_wait_mid_frame(lastTick, period);
	err = fast_view_set_sensor(on);
	u64 waitTicks = period * 3 / 2; // the rest of this frame and most of the next
	u64 waitTicksMax = (u64)(SWITCH_WAIT_MAX_MS * CPU_TICKS_PER_MSEC);
	if (waitTicks > waitTicksMax) {
		waitTicks = waitTicksMax;
	}
	cam_state_sleep_ticks(waitTicks);
	fast_view_update_frame_size();
	cam_config_stream_transfer(&err);
	CAM_TRY_AS("CAMU_ClearBuffer(S.port)", CAMU_ClearBuffer(g_camera.livePort));
	CAM_TRY_AS("CAMU_StartCapture(S.port)", CAMU_StartCapture(g_camera.livePort));
	capture_thread_resume();
	sensor_exposure_ask_guard(); // a look once the start's window has passed
	return err;
}
