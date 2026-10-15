#include "metering.h"
#include <3ds.h>
#include "cam_settings.h"
#include "math_util.h"
#include "raw_view.h"
#include "settings.h"

#define CELL_SIZE 2 // an RGGB cell is 2 x 2 pixels
#define METER_LARGE_AREA 40000 // pixels; a larger area is sampled more sparsely
#define METER_STEP_LARGE 16
#define METER_STEP_SMALL 4
#define CENTRE_WEIGHT 0.6f
#define SURROUND_WEIGHT 0.4f

typedef struct {
	int left;
	int top;
	int right;
	int bottom;
	bool centreWeighted;
} MeterArea;

static _Atomic float g_meterGreen = -1;
static _Atomic u32 g_meterFrames;
// Negative: no touch point.
static _Atomic float g_touchMeterX = -1;
static _Atomic float g_touchMeterY = -1;

// A spot around the touched point, else the Metering setting's window. It covers whole cells.
static MeterArea metering_area(int width, int height)
{
	MeterArea area;
	area.centreWeighted = false;
	float touchMeterX;
	float touchMeterY;
	if (metering_touch_point(&touchMeterX, &touchMeterY)) {
		int touchX = (int)(touchMeterX * width);
		int touchY = (int)(touchMeterY * height);
		area.left = touchX - width / METERING_TOUCH_SPOT_DIVISOR;
		area.right = touchX + width / METERING_TOUCH_SPOT_DIVISOR;
		area.top = touchY - height / METERING_TOUCH_SPOT_DIVISOR;
		area.bottom = touchY + height / METERING_TOUCH_SPOT_DIVISOR;
	} else {
		int windowIndex = math_clamp_int(g_settings.cam.aeWindow, 0, CAM_METERING_WINDOW_COUNT - 1);
		const CamMeteringWindow *window = &CAM_METERING_WINDOWS[windowIndex];
		area.left = window->x * width / CAM_FRAME_WIDTH;
		area.top = window->y * height / CAM_FRAME_HEIGHT;
		area.right = (window->x + window->width) * width / CAM_FRAME_WIDTH;
		area.bottom = (window->y + window->height) * height / CAM_FRAME_HEIGHT;
		area.centreWeighted = g_settings.cam.aeWindow == CAM_METERING_CENTRE;
	}
	area.left = math_clamp_int(area.left, 0, width - CELL_SIZE) & ~1;
	area.top = math_clamp_int(area.top, 0, height - CELL_SIZE) & ~1;
	area.right = math_clamp_int(area.right, area.left + CELL_SIZE, width);
	area.bottom = math_clamp_int(area.bottom, area.top + CELL_SIZE, height);
	return area;
}

// The meter reads green. Centre weighting mixes in the whole frame.
void metering_update(const u8 *frame, int width, int height, const RawGrid *grid)
{
	float wholeGreen = grid->cells ? (float)grid->greenSum / grid->cells : 0;
	MeterArea area = metering_area(width, height);
	int areaPixels = (area.right - area.left) * (area.bottom - area.top);
	int step = areaPixels > METER_LARGE_AREA ? METER_STEP_LARGE : METER_STEP_SMALL;
	u32 greenSum = 0;
	u32 samples = 0;
	for (int y = area.top; y + 1 < area.bottom; y += step) {
		for (int x = area.left; x + 1 < area.right; x += step) {
			greenSum += raw_view_above_black(raw_view_sample(frame, width, y, x + 1));
			samples++;
		}
	}
	float spotGreen = samples ? (float)greenSum / samples : 0;
	g_meterGreen = area.centreWeighted ? CENTRE_WEIGHT * spotGreen + SURROUND_WEIGHT * wholeGreen : spotGreen;
	g_meterFrames++;
}

float metering_green(void)
{
	return g_meterGreen;
}

u32 metering_frames(void)
{
	return g_meterFrames;
}

void metering_set_touch_point(float x, float y)
{
	g_touchMeterX = x;
	g_touchMeterY = y;
}

void metering_clear_touch_point(void)
{
	g_touchMeterY = -1;
	g_touchMeterX = -1;
}

bool metering_touch_point(float *x, float *y)
{
	*x = g_touchMeterX;
	*y = g_touchMeterY;
	return *x >= 0;
}
