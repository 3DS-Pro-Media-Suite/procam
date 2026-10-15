// Camera settings: the CamSettings struct, its value tables, defaults and clamping.
#pragma once
#include <3ds.h>

// Left and right as seen by the person holding the console: the eyes of a 3D picture. OUT1 is the lens
// behind the right half of the screen. Saved settings store these values, so they must not change.
typedef enum {
	CAM_OUTER_RIGHT = 0,
	CAM_INNER = 1,
	CAM_OUTER_LEFT = 2,
	CAM_COUNT
} CamId;

// Every field is an s8, so a settings struct can be stored, compared and edited generically.
typedef struct {
	// CamId. Outer right (OUT1) and inner share PORT_CAM1, so only one of them runs at a time. Outer left
	// (OUT2) has PORT_CAM2, so the two outer cameras can stream together.
	s8 camera;
	// Always SIZE_VGA: the stream is the full frame. Kept so saved settings still load.
	s8 size;
	// Always 0: the lenses cannot zoom, and there is no digital zoom. Kept so saved settings still load.
	s8 crop;
	s8 format; // CAM_FORMAT_RGB565 (converted by the hardware) or CAM_FORMAT_YUYV (converted by the caller)
	// CAMU_FrameRate. The "x to y" rates let the sensor slow down in the dark: the nearest thing the service
	// has to a shutter speed limit.
	s8 frameRate;
	s8 flip; // CAMU_Flip
	s8 noiseFilter; // bool
	s8 lensCorrection; // CAMU_LensCorrection
	s8 exposure; // exposure bias in EV, -CAM_EXPOSURE_BIAS_MAX .. CAM_EXPOSURE_BIAS_MAX
	s8 autoExposure; // bool. Off: the sensor holds its current exposure (AE lock).
	s8 aeWindow; // metering window of the auto exposure (CAM_METERING_*)
	s8 whiteBalance; // CAMU_WhiteBalance (0 = auto)
	s8 awbLock; // bool. With auto white balance: hold the current white balance.
	s8 awbWindow; // metering window of the auto white balance (CAM_METERING_*)
	s8 sharpness; // -CAM_SHARPNESS_MAX .. CAM_SHARPNESS_MAX
	s8 contrast; // CAMU_Contrast
	s8 photoMode; // CAMU_PhotoMode
	s8 effect; // CAMU_Effect
	// Exposure set on the sensor itself, raw mode only. The YUV path keeps the camera service's auto
	// exposure. In raw mode the sensor's own AE and AWB are always off.
	// CAM_EXPOSURE_AUTO: the app's exposure loop sets time and gain. CAM_EXPOSURE_SHUTTER_PRIORITY: the time
	// is fixed and the app adapts the gain. CAM_EXPOSURE_MANUAL: time and gain come from shutter and iso.
	s8 exposureMode;
	s8 shutter; // exposure time: index into CAM_SHUTTER_SECONDS
	// Analog gain, the same on every colour so the raw data stays neutral: index into CAM_ISO_VALUES.
	s8 iso;
} CamSettings;

enum {
	CAM_EXPOSURE_AUTO,
	CAM_EXPOSURE_SHUTTER_PRIORITY,
	CAM_EXPOSURE_MANUAL,
	CAM_EXPOSURE_MODE_COUNT
};

#define CAM_SHUTTER_COUNT 16
#define CAM_ISO_COUNT 13
#define CAM_DEFAULT_SHUTTER_INDEX 10 // 1/30 s
#define CAM_DEFAULT_ISO_INDEX 6 // ISO 400
extern const float CAM_SHUTTER_SECONDS[CAM_SHUTTER_COUNT]; // 1/32000 .. 1
extern const char *const CAM_SHUTTER_NAMES[CAM_SHUTTER_COUNT];
extern const u16 CAM_ISO_VALUES[CAM_ISO_COUNT]; // 100 .. 1600
extern const u8 CAM_ISO_GAIN_CODES[CAM_ISO_COUNT]; // analog gain codes with a 3-bit fraction: 8 = x1

#define CAM_FRAME_WIDTH 640
#define CAM_FRAME_HEIGHT 480
#define CAM_PIXEL_BYTES 2 // every frame format takes two bytes a pixel
#define CAM_FRAME_BYTES (CAM_FRAME_WIDTH * CAM_FRAME_HEIGHT * CAM_PIXEL_BYTES)

// Frame formats (CamSettings.format, cam_stream_format)
#define CAM_FORMAT_RGB565 0
#define CAM_FORMAT_YUYV 1
// 640 x 480 RAW10 in RGGB order. Each pixel takes two bytes: one with its 8 high bits, one with its 2 low
// bits.
#define CAM_FORMAT_RAW 2

// Metering windows of the auto exposure and the auto white balance (CamSettings.aeWindow and awbWindow).
enum {
	CAM_METERING_FULL,
	CAM_METERING_CENTRE, // the live view's meter weights it against the whole frame
	CAM_METERING_SPOT,
	CAM_METERING_TOP,
	CAM_METERING_BOTTOM,
	CAM_METERING_LEFT,
	CAM_METERING_RIGHT,
	CAM_METERING_WINDOW_COUNT
};

// A metering window in full frame coordinates.
typedef struct {
	s16 x;
	s16 y;
	s16 width;
	s16 height;
} CamMeteringWindow;

// Number of values of each camera service setting: the last libctru value + 1.
#define CAM_FRAME_RATE_COUNT (FRAME_RATE_30_TO_10 + 1)
#define CAM_FLIP_COUNT (FLIP_REVERSE + 1)
#define CAM_LENS_CORRECTION_COUNT (LENS_CORRECTION_ON_90 + 1)
#define CAM_WHITE_BALANCE_COUNT (WHITE_BALANCE_7000K + 1)
#define CAM_CONTRAST_COUNT (CONTRAST_PATTERN_11 + 1)
#define CAM_PHOTO_MODE_COUNT (PHOTO_MODE_LETTER + 1)
#define CAM_EFFECT_COUNT (EFFECT_SEPIA01 + 1)

#define CAM_EXPOSURE_BIAS_MAX 5
#define CAM_SHARPNESS_MAX 3

extern const CamMeteringWindow CAM_METERING_WINDOWS[CAM_METERING_WINDOW_COUNT];

// Value names for menus, indexed by the matching CamSettings field. The sidecar text has shorter names of its
// own for most of them.
extern const char *const CAM_CAMERA_NAMES[CAM_COUNT];
extern const char *const CAM_FRAME_RATE_NAMES[CAM_FRAME_RATE_COUNT];
extern const char *const CAM_FLIP_NAMES[CAM_FLIP_COUNT];
extern const char *const CAM_LENS_CORRECTION_NAMES[CAM_LENS_CORRECTION_COUNT];
extern const char *const CAM_WHITE_BALANCE_NAMES[CAM_WHITE_BALANCE_COUNT];
extern const char *const CAM_METERING_WINDOW_NAMES[CAM_METERING_WINDOW_COUNT];
extern const char *const CAM_PHOTO_MODE_NAMES[CAM_PHOTO_MODE_COUNT];
extern const char *const CAM_EFFECT_NAMES[CAM_EFFECT_COUNT];

void cam_settings_defaults(CamSettings *settings);
// Force every field into its legal range.
void cam_settings_clamp(CamSettings *settings);
