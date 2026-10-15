#include "cam_settings.h"
#include <3ds.h>
#include <string.h>
#include "math_util.h"

#define SPOT_WINDOW_SIZE 80

const CamMeteringWindow CAM_METERING_WINDOWS[CAM_METERING_WINDOW_COUNT] = {
        [CAM_METERING_FULL] = {0, 0, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT},
        [CAM_METERING_CENTRE] = {CAM_FRAME_WIDTH / 4, CAM_FRAME_HEIGHT / 4, CAM_FRAME_WIDTH / 2,
                                 CAM_FRAME_HEIGHT / 2},
        [CAM_METERING_SPOT] = {(CAM_FRAME_WIDTH - SPOT_WINDOW_SIZE) / 2,
                               (CAM_FRAME_HEIGHT - SPOT_WINDOW_SIZE) / 2, SPOT_WINDOW_SIZE, SPOT_WINDOW_SIZE},
        [CAM_METERING_TOP] = {0, 0, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT / 2},
        [CAM_METERING_BOTTOM] = {0, CAM_FRAME_HEIGHT / 2, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT / 2},
        [CAM_METERING_LEFT] = {0, 0, CAM_FRAME_WIDTH / 2, CAM_FRAME_HEIGHT},
        [CAM_METERING_RIGHT] = {CAM_FRAME_WIDTH / 2, 0, CAM_FRAME_WIDTH / 2, CAM_FRAME_HEIGHT},
};

const char *const CAM_CAMERA_NAMES[CAM_COUNT] = {"Outer right", "Inner", "Outer left"};
const char *const CAM_FRAME_RATE_NAMES[CAM_FRAME_RATE_COUNT] = {
        "15 fps",      "15 to 5 fps", "15 to 2 fps", "10 fps",       "8.5 fps",      "5 fps",       "20 fps",
        "20 to 5 fps", "30 fps",      "30 to 5 fps", "15 to 10 fps", "20 to 10 fps", "30 to 10 fps"};
const char *const CAM_FLIP_NAMES[CAM_FLIP_COUNT] = {"None", "Mirror", "Upside down", "Rotate 180"};
const char *const CAM_LENS_CORRECTION_NAMES[CAM_LENS_CORRECTION_COUNT] = {"Off", "Light (70)", "Strong (90)"};
const char *const CAM_WHITE_BALANCE_NAMES[CAM_WHITE_BALANCE_COUNT] = {
        "Auto", "Tungsten 3200 K", "Fluorescent 4150 K", "Daylight 5200 K", "Cloudy 6000 K", "Shade 7000 K"};
const char *const CAM_METERING_WINDOW_NAMES[CAM_METERING_WINDOW_COUNT] = {
        "Multi (whole frame)", "Centre weighted", "Spot",      "Top half",
        "Bottom half",         "Left half",       "Right half"};
const char *const CAM_PHOTO_MODE_NAMES[CAM_PHOTO_MODE_COUNT] = {"Normal", "Portrait", "Landscape", "Night",
                                                                "Text"};
const char *const CAM_EFFECT_NAMES[CAM_EFFECT_COUNT] = {"Standard", "Monochrome",    "Sepia",
                                                        "Negative", "Negative film", "Sepia warm"};

// Up to 1 s: much longer times make the camera interface raise buffer errors and leave the sensor unable to
// answer until the camera driver is reset. The shortest times are fractions of a sensor line.
const float CAM_SHUTTER_SECONDS[CAM_SHUTTER_COUNT] = {
        1 / 32000.f, 1 / 16000.f, 1 / 8000.f, 1 / 4000.f, 1 / 2000.f, 1 / 1000.f, 1 / 500.f, 1 / 250.f,
        1 / 125.f,   1 / 60.f,    1 / 30.f,   1 / 15.f,   1 / 8.f,    1 / 4.f,    1 / 2.f,   1};
const char *const CAM_SHUTTER_NAMES[CAM_SHUTTER_COUNT] = {
        "1/32000", "1/16000", "1/8000", "1/4000", "1/2000", "1/1000", "1/500", "1/250",
        "1/125",   "1/60",    "1/30",   "1/15",   "1/8",    "1/4",    "1/2",   "1\""};

// Third-stop ISO steps, each with the nearest even gain code. 150 instead of 160: the gain has no step near
// x1.6. 1600 is the highest gain the sensor has.
const u16 CAM_ISO_VALUES[CAM_ISO_COUNT] = {100, 125, 150, 200,  250,  320, 400,
                                           500, 640, 800, 1000, 1250, 1600};
const u8 CAM_ISO_GAIN_CODES[CAM_ISO_COUNT] = {8, 10, 12, 16, 20, 26, 32, 40, 52, 64, 80, 100, 126};

void cam_settings_defaults(CamSettings *settings)
{
	memset(settings, 0, sizeof *settings);
	settings->size = SIZE_VGA;
	// YUV gives 8-bit luma instead of RGB565's 5 or 6 bits, and the camera module keeps delivering YUV once
	// it has streamed it.
	settings->format = CAM_FORMAT_YUYV;
	// 30 fps in good light, down to 5 fps (1/5 s exposures) in the dark instead of a higher gain.
	settings->frameRate = FRAME_RATE_30_TO_5;
	settings->noiseFilter = 1;
	settings->lensCorrection = LENS_CORRECTION_NORMAL;
	settings->autoExposure = 1;
	settings->contrast = CONTRAST_NORMAL;
	settings->exposureMode = CAM_EXPOSURE_AUTO;
	settings->shutter = CAM_DEFAULT_SHUTTER_INDEX;
	settings->iso = CAM_DEFAULT_ISO_INDEX;
}

void cam_settings_clamp(CamSettings *settings)
{
	settings->camera = math_clamp_int(settings->camera, 0, CAM_COUNT - 1);
	// Always the full frame, which photos are taken at: a smaller size is only the module scaling the view
	// down. The lenses cannot zoom, so there is no digital zoom either.
	settings->size = SIZE_VGA;
	settings->crop = 0;
	if (settings->format != CAM_FORMAT_RGB565) {
		settings->format = CAM_FORMAT_YUYV;
	}
	settings->frameRate = math_clamp_int(settings->frameRate, 0, CAM_FRAME_RATE_COUNT - 1);
	settings->flip = math_clamp_int(settings->flip, 0, CAM_FLIP_COUNT - 1);
	settings->noiseFilter = settings->noiseFilter != 0;
	settings->lensCorrection = math_clamp_int(settings->lensCorrection, 0, CAM_LENS_CORRECTION_COUNT - 1);
	settings->exposure = math_clamp_int(settings->exposure, -CAM_EXPOSURE_BIAS_MAX, CAM_EXPOSURE_BIAS_MAX);
	settings->autoExposure = settings->autoExposure != 0;
	settings->aeWindow = math_clamp_int(settings->aeWindow, 0, CAM_METERING_WINDOW_COUNT - 1);
	settings->whiteBalance = math_clamp_int(settings->whiteBalance, 0, CAM_WHITE_BALANCE_COUNT - 1);
	settings->awbLock = settings->awbLock != 0;
	settings->awbWindow = math_clamp_int(settings->awbWindow, 0, CAM_METERING_WINDOW_COUNT - 1);
	settings->sharpness = math_clamp_int(settings->sharpness, -CAM_SHARPNESS_MAX, CAM_SHARPNESS_MAX);
	settings->contrast = math_clamp_int(settings->contrast, 0, CAM_CONTRAST_COUNT - 1);
	settings->photoMode = math_clamp_int(settings->photoMode, 0, CAM_PHOTO_MODE_COUNT - 1);
	settings->effect = math_clamp_int(settings->effect, 0, CAM_EFFECT_COUNT - 1);
	settings->exposureMode = math_clamp_int(settings->exposureMode, 0, CAM_EXPOSURE_MODE_COUNT - 1);
	settings->shutter = math_clamp_int(settings->shutter, 0, CAM_SHUTTER_COUNT - 1);
	settings->iso = math_clamp_int(settings->iso, 0, CAM_ISO_COUNT - 1);
}
