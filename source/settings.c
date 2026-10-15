#include "settings.h"
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "cam_settings.h"
#include "look.h"
#include "math_util.h"
#include "toast.h"
#include "ui_text.h"

#define SETTINGS_FILE_PATH APP_DIR "/procam.cfg"
#define LOOKS_DIR APP_DIR "/looks"

// Change it whenever the layout of Settings changes: an older file is then replaced by the defaults once.
#define SETTINGS_FILE_MAGIC 0x50434D37u

// SettingsFile.flags bits.
#define FLAG_SCREENS_SWAPPED 0x01
#define FLAG_HOT_PIXEL_FIX_OFF 0x02
#define FLAG_FAST_LIVE_VIEW_OFF 0x04
// Set in every file this version writes. A file without them is older and is ignored.
#define FLAG_CURRENT_VERSION 0xC0

// MERGE and HDR: frames per camera.
#define DEFAULT_BURST_FRAMES_INDEX 2 // 8 frames

// HDR's option row holds the gap and the frames in one value: gap index * BURST_FRAME_CHOICE_COUNT + frames
// index.
#define DEFAULT_HDR_GAP_EV 2
#define HDR_OPTION_COUNT (HDR_GAP_COUNT * BURST_FRAME_CHOICE_COUNT)
#define DEFAULT_HDR_OPTION \
	((DEFAULT_HDR_GAP_EV - HDR_GAP_MIN_EV) * BURST_FRAME_CHOICE_COUNT + DEFAULT_BURST_FRAMES_INDEX)

#define SELF_TIMER_OFF 0
#define SLOT_NUMBER_BIAS (-1) // the slots are shown from 1

// The settings file: the last settings, the preset slots and the app switches.
typedef struct {
	u32 magic;
	u32 size;
	Settings last;
	Settings slot[PRESET_SLOT_COUNT];
	u8 valid[PRESET_SLOT_COUNT];
	u8 flags; // saved app switches and file version bits
} SettingsFile;

// The frames per camera of each Settings.burstFrames value.
static const int BURST_FRAMES_PER_CAMERA[BURST_FRAME_CHOICE_COUNT] = {4, 6, 8, 12};

Settings g_settings;
static SettingsFile g_settingsFile;
bool g_screensSwapped;
bool g_isNew3ds;
bool g_hotPixelFix = true;
bool g_fastLiveView = true;

void settings_defaults(Settings *settings)
{
	memset(settings, 0, sizeof *settings);
	cam_settings_defaults(&settings->cam);
	settings->shutterSound = 1;
	settings->histogram = 1;
	settings->hud = HUD_FULL;
	settings->review = 1;
	settings->burstFrames = DEFAULT_BURST_FRAMES_INDEX;
	settings->hdrGap = DEFAULT_HDR_GAP_EV;
}

// Every setting is a row: its range, its value names and its default. The Q menu shows sixteen of them as
// tiles. The setup menu (START) lists the rest by page.

static const char *const OFF_ON_NAMES[] = {"Off", "On"};
static const char *const OFF_ON_SHORT_NAMES[] = {"OFF", "ON"};
static const char *const CAMERA_NAMES[CAMERA_CHOICE_COUNT] = {"Outer (rear)", "Inner (selfie)"};
const char *const CAMERA_SHORT_NAMES[CAMERA_CHOICE_COUNT] = {"OUTER", "INNER"};
static const char *const MODE_NAMES[MODE_COUNT] = {
        "Photo: one raw frame (DNG + JPG)", "Merge: a burst into one cleaner photo",
        "HDR: 2 cameras, 2 exposures, 1 photo", "3D: a stereo pair (outer cameras)",
        "Bracket: an exposure series of photos"};
const char *const MODE_SHORT_NAMES[MODE_COUNT] = {"PHOTO", "MERGE", "HDR", "3D", "BRACKET"};
static const char *const BURST_FRAME_NAMES[BURST_FRAME_CHOICE_COUNT] = {"4 frames", "6 frames", "8 frames",
                                                                        "12 frames"};
static const char *const BURST_FRAME_SHORT_NAMES[BURST_FRAME_CHOICE_COUNT] = {"4", "6", "8", "12"};
static const char *const HDR_GAP_SHORT_NAMES[HDR_GAP_COUNT] = {"1 EV", "2 EV", "3 EV"};
static const char *const HDR_OPTION_NAMES[HDR_OPTION_COUNT] = {
        "1 EV (A +1, B 0), 4 frames",  "1 EV (A +1, B 0), 6 frames",   "1 EV (A +1, B 0), 8 frames",
        "1 EV (A +1, B 0), 12 frames", "2 EV (A +1, B -1), 4 frames",  "2 EV (A +1, B -1), 6 frames",
        "2 EV (A +1, B -1), 8 frames", "2 EV (A +1, B -1), 12 frames", "3 EV (A +2, B -1), 4 frames",
        "3 EV (A +2, B -1), 6 frames", "3 EV (A +2, B -1), 8 frames",  "3 EV (A +2, B -1), 12 frames"};
static const char *const HDR_OPTION_SHORT_NAMES[HDR_OPTION_COUNT] = {"1EV 4", "1EV 6", "1EV 8", "1EV 12",
                                                                     "2EV 4", "2EV 6", "2EV 8", "2EV 12",
                                                                     "3EV 4", "3EV 6", "3EV 8", "3EV 12"};
const char *const BRACKET_SERIES_NAMES[BRACKET_SERIES_CHOICE_COUNT] = {
        "3 shots, " UTF8_PLUS_MINUS_SIGN "1 EV", "3 shots, " UTF8_PLUS_MINUS_SIGN "2 EV",
        "5 shots, -2 to +2 EV"};
static const char *const BRACKET_SERIES_SHORT_NAMES[BRACKET_SERIES_CHOICE_COUNT] = {
        "3 " UTF8_PLUS_MINUS_SIGN "1", "3 " UTF8_PLUS_MINUS_SIGN "2", "5 " UTF8_PLUS_MINUS_SIGN "2"};
static const char *const PHOTO_SIZE_NAMES[PHOTO_SIZE_COUNT] = {
        "Full 640" UTF8_MULTIPLICATION_SIGN "480",
        "Half 320" UTF8_MULTIPLICATION_SIGN "240 (nearest neighbour)",
        "Quarter 160" UTF8_MULTIPLICATION_SIGN "120 (nearest neighbour)"};
static const char *const PHOTO_SIZE_SHORT_NAMES[PHOTO_SIZE_COUNT] = {"FULL", "HALF", "QUARTER"};
const char *const FRAME_RATE_SHORT_NAMES[CAM_FRAME_RATE_COUNT] = {
        "15", "15-5", "15-2", "10", "8.5", "5", "20", "20-5", "30", "30-5", "15-10", "20-10", "30-10"};
static const char *const OUTPUT_FORMAT_NAMES[OUTPUT_FORMAT_COUNT] = {"RGB565", "YUV 4:2:2"};
const char *const OUTPUT_FORMAT_SHORT_NAMES[OUTPUT_FORMAT_COUNT] = {"RGB", "YUV"};
static const char *const AUTO_EXPOSURE_NAMES[] = {"Locked", "Auto"};
static const char *const PHOTO_FORMAT_NAMES[PHOTO_FORMAT_COUNT] = {"RAW + JPG", "JPG only",
                                                                   "RAW only (DNG, no look)"};
static const char *const PHOTO_FORMAT_SHORT_NAMES[PHOTO_FORMAT_COUNT] = {"RAW+JPG", "JPG", "RAW"};
static const char *const EXPOSURE_MODE_NAMES[CAM_EXPOSURE_MODE_COUNT] = {"Auto (Pro Cam's exposure loop)",
                                                                         "Shutter priority", "Manual"};
static const char *const EXPOSURE_MODE_SHORT_NAMES[CAM_EXPOSURE_MODE_COUNT] = {"AUTO", "S", "M"};
static const char *const ISO_NAMES[CAM_ISO_COUNT] = {"100", "125", "150", "200",  "250",  "320", "400",
                                                     "500", "640", "800", "1000", "1250", "1600"};
const char *const METERING_SHORT_NAMES[CAM_METERING_WINDOW_COUNT] = {"MULTI",  "CENTER", "SPOT", "TOP",
                                                                     "BOTTOM", "LEFT",   "RIGHT"};
const char *const WHITE_BALANCE_SHORT_NAMES[CAM_WHITE_BALANCE_COUNT] = {"AUTO",  "3200K", "4150K",
                                                                        "5200K", "6000K", "7000K"};
static const char *const SELF_TIMER_NAMES[SELF_TIMER_CHOICE_COUNT] = {"Off", "2 s", "5 s", "10 s"};
static const char *const SELF_TIMER_SHORT_NAMES[SELF_TIMER_CHOICE_COUNT] = {"OFF", "2s", "5s", "10s"};
static const char *const HUD_NAMES[HUD_LEVEL_COUNT] = {"Off", "Minimal", "Full"};

// A setup menu row that turns a Settings field on or off.
#define BOOL_ROW(rowLabel, field, defaultOn) \
	{.label = rowLabel,                      \
	 .kind = ROW_BOOL,                       \
	 .offset = SETTING_OFFSET(field),        \
	 .max = 1,                               \
	 .defaultValue = defaultOn,              \
	 .names = OFF_ON_NAMES}

// A setup menu row that runs an action when chosen.
#define ACTION_ROW(rowLabel, rowAction) {.label = rowLabel, .kind = ROW_ACTION, .action = rowAction}

// Not const: settings_load_looks fills in the LOOK tile, settings_update_mode_row the option tile.
SettingRow g_quickRows[QUICK_TILE_COUNT] = {
        {.label = "Camera",
         .tag = "CAM",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(cam.camera),
         .max = CAMERA_CHOICE_COUNT - 1,
         .defaultValue = CAM_OUTER_RIGHT,
         .names = CAMERA_NAMES,
         .shortNames = CAMERA_SHORT_NAMES},
        [QUICK_TILE_MODE] = {.label = "Mode (what the shutter takes)",
                             .tag = "MODE",
                             .kind = ROW_ENUM,
                             .offset = SETTING_OFFSET(mode),
                             .max = MODE_COUNT - 1,
                             .defaultValue = MODE_PHOTO,
                             .names = MODE_NAMES,
                             .shortNames = MODE_SHORT_NAMES},
        [QUICK_TILE_OPTION] = {.label = "", .tag = "OPTION", .kind = ROW_ACTION},
        {.label = "Live view 60 fps (raw, Photo mode)",
         .tag = "60 FPS",
         .kind = ROW_BOOL,
         .max = 1,
         .defaultValue = 1,
         .names = OFF_ON_NAMES,
         .shortNames = OFF_ON_SHORT_NAMES,
         .action = ACTION_FAST_LIVE_VIEW},
        [QUICK_TILE_SHUTTER] = {.label = "Shutter (exposure time)",
                                .tag = "SHUTTER",
                                .kind = ROW_ENUM,
                                .offset = SETTING_OFFSET(cam.shutter),
                                .max = CAM_SHUTTER_COUNT - 1,
                                .defaultValue = CAM_DEFAULT_SHUTTER_INDEX,
                                .names = CAM_SHUTTER_NAMES},
        [QUICK_TILE_EV] = {.label = "Exposure compensation",
                           .tag = "EV",
                           .kind = ROW_INT,
                           .offset = SETTING_OFFSET(cam.exposure),
                           .min = -CAM_EXPOSURE_BIAS_MAX,
                           .max = CAM_EXPOSURE_BIAS_MAX,
                           .defaultValue = 0},
        [QUICK_TILE_EXPOSURE_MODE] = {.label = "Exposure mode",
                                      .tag = "EXPOSURE",
                                      .kind = ROW_ENUM,
                                      .offset = SETTING_OFFSET(cam.exposureMode),
                                      .max = CAM_EXPOSURE_MODE_COUNT - 1,
                                      .defaultValue = CAM_EXPOSURE_AUTO,
                                      .names = EXPOSURE_MODE_NAMES,
                                      .shortNames = EXPOSURE_MODE_SHORT_NAMES},
        {.label = "Metering",
         .tag = "METER",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(cam.aeWindow),
         .max = CAM_METERING_WINDOW_COUNT - 1,
         .defaultValue = 0,
         .names = CAM_METERING_WINDOW_NAMES,
         .shortNames = METERING_SHORT_NAMES},
        {.label = "White balance",
         .tag = "WB",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(cam.whiteBalance),
         .max = CAM_WHITE_BALANCE_COUNT - 1,
         .defaultValue = WHITE_BALANCE_AUTO,
         .names = CAM_WHITE_BALANCE_NAMES,
         .shortNames = WHITE_BALANCE_SHORT_NAMES},
        [QUICK_TILE_ISO] = {.label = "ISO (analog gain)",
                            .tag = "ISO",
                            .kind = ROW_ENUM,
                            .offset = SETTING_OFFSET(cam.iso),
                            .max = CAM_ISO_COUNT - 1,
                            .defaultValue = CAM_DEFAULT_ISO_INDEX,
                            .names = ISO_NAMES},
        {.label = "Sharpness",
         .tag = "SHARP",
         .kind = ROW_INT,
         .offset = SETTING_OFFSET(cam.sharpness),
         .min = -CAM_SHARPNESS_MAX,
         .max = CAM_SHARPNESS_MAX,
         .defaultValue = 0},
        {.label = "Noise reduction",
         .tag = "NR",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(cam.noiseFilter),
         .max = 1,
         .defaultValue = 1,
         .names = OFF_ON_NAMES,
         .shortNames = OFF_ON_SHORT_NAMES},
        {.label = "Photo format",
         .tag = "FORMAT",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(photoFormat),
         .max = PHOTO_FORMAT_COUNT - 1,
         .defaultValue = PHOTO_FORMAT_RAW_AND_JPG,
         .names = PHOTO_FORMAT_NAMES,
         .shortNames = PHOTO_FORMAT_SHORT_NAMES},
        [QUICK_TILE_LOOK] = {.label = "Look (JPG and live view)",
                             .tag = "LOOK",
                             .kind = ROW_ENUM,
                             .offset = SETTING_OFFSET(lookIndex)},
        {.label = "Photo size",
         .tag = "SIZE",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(photoSize),
         .max = PHOTO_SIZE_COUNT - 1,
         .defaultValue = PHOTO_SIZE_FULL,
         .names = PHOTO_SIZE_NAMES,
         .shortNames = PHOTO_SIZE_SHORT_NAMES},
        {.label = "Self-timer",
         .tag = "TIMER",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(timer),
         .max = SELF_TIMER_CHOICE_COUNT - 1,
         .defaultValue = SELF_TIMER_OFF,
         .names = SELF_TIMER_NAMES,
         .shortNames = SELF_TIMER_SHORT_NAMES},
};

// The option tile of each mode: the settings of that mode, in their only place. HDR's frames are MERGE's
// frames too (one field). Photo and 3D have no option: their tile stays, greyed, so the grid does not move.
static const SettingRow MODE_OPTION_ROWS[MODE_COUNT] = {
        [MODE_PHOTO] = {.label = "Photo: no option of its own", .tag = "OPTION", .kind = ROW_ACTION},
        [MODE_MERGE] = {.label = "Merge: frames per camera (HDR uses them too)",
                        .tag = "FRAMES",
                        .kind = ROW_ENUM,
                        .offset = SETTING_OFFSET(burstFrames),
                        .max = BURST_FRAME_CHOICE_COUNT - 1,
                        .defaultValue = DEFAULT_BURST_FRAMES_INDEX,
                        .names = BURST_FRAME_NAMES,
                        .shortNames = BURST_FRAME_SHORT_NAMES},
        [MODE_HDR] = {.label = "HDR: gap and frames per camera (Merge's too)",
                      .tag = "GAP+FR",
                      .kind = ROW_ENUM,
                      .offset = SETTING_OFFSET(hdrGap),
                      .max = HDR_OPTION_COUNT - 1,
                      .defaultValue = DEFAULT_HDR_OPTION,
                      .names = HDR_OPTION_NAMES,
                      .shortNames = HDR_OPTION_SHORT_NAMES},
        [MODE_3D] = {.label = "3D: no option of its own", .tag = "OPTION", .kind = ROW_ACTION},
        [MODE_BRACKET] = {.label = "Bracket: shots and steps",
                          .tag = "SERIES",
                          .kind = ROW_ENUM,
                          .offset = SETTING_OFFSET(bracketSeries),
                          .max = BRACKET_SERIES_CHOICE_COUNT - 1,
                          .defaultValue = 0,
                          .names = BRACKET_SERIES_NAMES,
                          .shortNames = BRACKET_SERIES_SHORT_NAMES},
};

// The row L toggles, in SHOOTING_ROWS.
#define SHOOTING_ROW_AE_LOCK 3

// Rows marked YUV mode set the camera service's own image processing, which the raw stream bypasses.
static const SettingRow SHOOTING_ROWS[] = {
        {.label = "Vignetting correction (YUV mode)",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(cam.lensCorrection),
         .max = CAM_LENS_CORRECTION_COUNT - 1,
         .defaultValue = LENS_CORRECTION_ON_70,
         .names = CAM_LENS_CORRECTION_NAMES},
        {.label = "Colour effect (YUV mode)",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(cam.effect),
         .max = CAM_EFFECT_COUNT - 1,
         .defaultValue = EFFECT_NONE,
         .names = CAM_EFFECT_NAMES},
        {.label = "Frame rate (YUV mode)",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(cam.frameRate),
         .max = CAM_FRAME_RATE_COUNT - 1,
         .defaultValue = FRAME_RATE_30_TO_5,
         .names = CAM_FRAME_RATE_NAMES},
        [SHOOTING_ROW_AE_LOCK] = {.label = "Auto exposure lock",
                                  .kind = ROW_ENUM,
                                  .offset = SETTING_OFFSET(cam.autoExposure),
                                  .max = 1,
                                  .defaultValue = 1,
                                  .names = AUTO_EXPOSURE_NAMES},
        {.label = "Tone (contrast, YUV mode)",
         .kind = ROW_INT,
         .offset = SETTING_OFFSET(cam.contrast),
         .max = CAM_CONTRAST_COUNT - 1,
         .defaultValue = CONTRAST_NORMAL,
         .bias = CONTRAST_NORMAL},
        {.label = "Output format (YUV mode)",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(cam.format),
         .max = OUTPUT_FORMAT_COUNT - 1,
         .defaultValue = CAM_FORMAT_YUYV,
         .names = OUTPUT_FORMAT_NAMES},
        {.label = "Scene mode (YUV mode)",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(cam.photoMode),
         .max = CAM_PHOTO_MODE_COUNT - 1,
         .defaultValue = PHOTO_MODE_NORMAL,
         .names = CAM_PHOTO_MODE_NAMES},
        BOOL_ROW("White balance lock (YUV mode)", cam.awbLock, 0),
        {.label = "WB area (YUV mode)",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(cam.awbWindow),
         .max = CAM_METERING_WINDOW_COUNT - 1,
         .defaultValue = 0,
         .names = CAM_METERING_WINDOW_NAMES},
        {.label = "Flip",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(cam.flip),
         .max = CAM_FLIP_COUNT - 1,
         .defaultValue = FLIP_NONE,
         .names = CAM_FLIP_NAMES},
        BOOL_ROW("Shutter sound", shutterSound, 1),
        BOOL_ROW("Image review", review, 1),
};

static const SettingRow DISPLAY_ROWS[] = {
        {.label = "Information",
         .kind = ROW_ENUM,
         .offset = SETTING_OFFSET(hud),
         .max = HUD_LEVEL_COUNT - 1,
         .defaultValue = HUD_FULL,
         .names = HUD_NAMES},
        BOOL_ROW("Histogram", histogram, 1),
        BOOL_ROW("Framing grid", grid, 0),
        BOOL_ROW("Highlight / shadow warning", zebra, 0),
        BOOL_ROW("Focus peaking", peaking, 0),
};

static const SettingRow PRESET_ROWS[] = {
        {.label = "Custom slot",
         .kind = ROW_INT,
         .offset = SETTING_OFFSET(slot),
         .max = PRESET_SLOT_COUNT - 1,
         .defaultValue = 0,
         .bias = SLOT_NUMBER_BIAS},
        ACTION_ROW("Save settings to slot", ACTION_SAVE_PRESET),
        ACTION_ROW("Load settings from slot", ACTION_LOAD_PRESET),
        ACTION_ROW("Reset all settings", ACTION_RESET),
};

static const SettingRow SYSTEM_ROWS[] = {
        ACTION_ROW("Swap screens (viewfinder on touch screen)  ZL", ACTION_SWAP_SCREENS),
        ACTION_ROW("Open the gallery", ACTION_GALLERY),
        ACTION_ROW("Edit the current look (recipe editor)", ACTION_LOOK_EDITOR),
        ACTION_ROW("Hot pixel fix in the raw (DNG): on / off", ACTION_HOT_PIXEL_FIX),
        ACTION_ROW("Camera information", ACTION_INFO),
        ACTION_ROW("Exit Pro Cam", ACTION_EXIT),
};

const SettingPage SETTINGS_PAGES[SETTINGS_PAGE_COUNT] = {
        [SETTINGS_PAGE_SHOOTING] = {"SHOOTING", SHOOTING_ROWS, ARRAY_COUNT(SHOOTING_ROWS)},
        [SETTINGS_PAGE_DISPLAY] = {"DISPLAY", DISPLAY_ROWS, ARRAY_COUNT(DISPLAY_ROWS)},
        [SETTINGS_PAGE_PRESETS] = {"PRESETS", PRESET_ROWS, ARRAY_COUNT(PRESET_ROWS)},
        [SETTINGS_PAGE_SYSTEM] = {"SYSTEM", SYSTEM_ROWS, ARRAY_COUNT(SYSTEM_ROWS)},
};

const SettingRow *settings_ae_lock_row(void)
{
	return &SHOOTING_ROWS[SHOOTING_ROW_AE_LOCK];
}

static s8 *row_field(const SettingRow *row, Settings *settings)
{
	return (s8 *)((char *)settings + row->offset);
}

bool settings_row_is_switch(const SettingRow *row)
{
	return row->kind != ROW_ACTION && row->action != ACTION_NONE;
}

bool settings_row_is_hdr(const SettingRow *row)
{
	return row->kind == ROW_ENUM && row->offset == SETTING_OFFSET(hdrGap);
}

bool settings_row_is_camera_setting(const SettingRow *row)
{
	size_t first = offsetof(Settings, cam);
	return row->offset >= first && row->offset < first + sizeof(CamSettings);
}

int settings_hdr_option_burst_index(int value)
{
	return value % BURST_FRAME_CHOICE_COUNT;
}

int settings_burst_frames_per_camera(int index)
{
	return BURST_FRAMES_PER_CAMERA[math_clamp_int(index, 0, BURST_FRAME_CHOICE_COUNT - 1)];
}

const char *settings_hdr_gap_name(const Settings *settings)
{
	int gapEv = math_clamp_int(settings->hdrGap, HDR_GAP_MIN_EV, HDR_GAP_MAX_EV);
	return HDR_GAP_SHORT_NAMES[gapEv - HDR_GAP_MIN_EV];
}

const char *settings_bracket_series_name(const Settings *settings)
{
	int series = math_clamp_int(settings->bracketSeries, 0, BRACKET_SERIES_CHOICE_COUNT - 1);
	return BRACKET_SERIES_SHORT_NAMES[series];
}

static int hdr_option_get(const Settings *settings)
{
	int gapIndex = math_clamp_int(settings->hdrGap, HDR_GAP_MIN_EV, HDR_GAP_MAX_EV) - HDR_GAP_MIN_EV;
	int framesIndex = math_clamp_int(settings->burstFrames, 0, BURST_FRAME_CHOICE_COUNT - 1);
	return gapIndex * BURST_FRAME_CHOICE_COUNT + framesIndex;
}

static void hdr_option_set(Settings *settings, int value)
{
	settings->hdrGap = (s8)(HDR_GAP_MIN_EV + value / BURST_FRAME_CHOICE_COUNT);
	settings->burstFrames = (s8)settings_hdr_option_burst_index(value);
}

int settings_row_get(const SettingRow *row, Settings *settings)
{
	if (settings_row_is_switch(row)) {
		return row->action == ACTION_FAST_LIVE_VIEW ? g_fastLiveView : 0;
	}
	if (settings_row_is_hdr(row)) {
		return hdr_option_get(settings);
	}
	return *row_field(row, settings);
}

void settings_row_set(const SettingRow *row, Settings *settings, int value)
{
	if (settings_row_is_hdr(row)) {
		hdr_option_set(settings, value);
	} else {
		*row_field(row, settings) = (s8)value;
	}
}

static void clamp_row(const SettingRow *row, Settings *settings)
{
	if (row->kind == ROW_ACTION || settings_row_is_switch(row)) {
		return;
	}
	if (settings_row_is_hdr(row)) {
		settings->hdrGap = (s8)math_clamp_int(settings->hdrGap, HDR_GAP_MIN_EV, HDR_GAP_MAX_EV);
		settings->burstFrames = (s8)math_clamp_int(settings->burstFrames, 0, BURST_FRAME_CHOICE_COUNT - 1);
		return;
	}
	s8 *value = row_field(row, settings);
	if (*value < row->min) {
		*value = row->min;
	}
	if (*value > row->max) {
		*value = row->max;
	}
}

// Every value inside its row's range, so that a damaged file cannot index past a name table.
static void clamp_settings(Settings *settings)
{
	cam_settings_clamp(&settings->cam);
	for (int i = 0; i < QUICK_TILE_COUNT; i++) {
		clamp_row(&g_quickRows[i], settings);
	}
	// Every mode's option: the option tile holds only the current one.
	for (int mode = 0; mode < MODE_COUNT; mode++) {
		clamp_row(&MODE_OPTION_ROWS[mode], settings);
	}
	for (int page = 0; page < SETTINGS_PAGE_COUNT; page++) {
		for (int i = 0; i < SETTINGS_PAGES[page].count; i++) {
			clamp_row(&SETTINGS_PAGES[page].rows[i], settings);
		}
	}
}

static void use_loaded_file(void)
{
	clamp_settings(&g_settingsFile.last);
	for (int i = 0; i < PRESET_SLOT_COUNT; i++) {
		clamp_settings(&g_settingsFile.slot[i]);
	}
	g_settings = g_settingsFile.last;
	// An AE lock is for the scene in front of the camera, not for the next launch.
	g_settings.cam.autoExposure = 1;
	g_screensSwapped = g_settingsFile.flags & FLAG_SCREENS_SWAPPED;
	g_hotPixelFix = !(g_settingsFile.flags & FLAG_HOT_PIXEL_FIX_OFF);
	g_fastLiveView = !(g_settingsFile.flags & FLAG_FAST_LIVE_VIEW_OFF);
}

static bool is_current_file(const SettingsFile *settingsFile)
{
	bool sameMagic = settingsFile->magic == SETTINGS_FILE_MAGIC;
	bool sameSize = settingsFile->size == sizeof *settingsFile;
	bool currentVersion = (settingsFile->flags & FLAG_CURRENT_VERSION) == FLAG_CURRENT_VERSION;
	return sameMagic && sameSize && currentVersion;
}

// Any other file keeps the defaults, and the next save replaces it.
void settings_load(void)
{
	memset(&g_settingsFile, 0, sizeof g_settingsFile);
	settings_defaults(&g_settings);
	FILE *file = fopen(SETTINGS_FILE_PATH, "rb");
	if (!file) {
		return;
	}
	SettingsFile loaded;
	bool complete = fread(&loaded, 1, sizeof loaded, file) == sizeof loaded;
	if (complete && is_current_file(&loaded)) {
		g_settingsFile = loaded;
		use_loaded_file();
	}
	fclose(file);
}

void settings_save(void)
{
	mkdir(HOMEBREW_DIR, FOLDER_MODE);
	mkdir(APP_DIR, FOLDER_MODE);
	g_settingsFile.magic = SETTINGS_FILE_MAGIC;
	g_settingsFile.size = sizeof g_settingsFile;
	g_settingsFile.last = g_settings;
	u8 swapFlag = g_screensSwapped ? FLAG_SCREENS_SWAPPED : 0;
	u8 hotPixelFlag = g_hotPixelFix ? 0 : FLAG_HOT_PIXEL_FIX_OFF;
	u8 fastLiveViewFlag = g_fastLiveView ? 0 : FLAG_FAST_LIVE_VIEW_OFF;
	g_settingsFile.flags = swapFlag | hotPixelFlag | fastLiveViewFlag | FLAG_CURRENT_VERSION;
	FILE *file = fopen(SETTINGS_FILE_PATH, "wb");
	if (!file) {
		toast_show("Cannot write %s", SETTINGS_FILE_PATH);
		return;
	}
	fwrite(&g_settingsFile, 1, sizeof g_settingsFile, file);
	fclose(file);
}

void settings_save_preset(int slot)
{
	g_settingsFile.slot[slot] = g_settings;
	g_settingsFile.valid[slot] = 1;
	settings_save();
}

bool settings_load_preset(int slot)
{
	if (!g_settingsFile.valid[slot]) {
		return false;
	}
	g_settings = g_settingsFile.slot[slot];
	g_settings.slot = (s8)slot;
	// A preset never loads with the exposure locked.
	g_settings.cam.autoExposure = 1;
	return true;
}

void settings_value_name(const SettingRow *row, int stored, char *out, size_t outSize, bool shortForm)
{
	switch (row->kind) {
	case ROW_ENUM:
	case ROW_BOOL: {
		bool useShortNames = shortForm && row->shortNames;
		const char *const *names = useShortNames ? row->shortNames : row->names;
		snprintf(out, outSize, "%s", names[stored - row->min]);
		break;
	}
	case ROW_INT: {
		int shown = stored - row->bias;
		bool hasNegativeValues = row->min - row->bias < 0;
		if (hasNegativeValues) {
			snprintf(out, outSize, shown ? "%+d" : "0", shown);
		} else {
			snprintf(out, outSize, "%d", shown);
		}
		break;
	}
	case ROW_ACTION:
		snprintf(out, outSize, "%s", "");
		break;
	}
}

void settings_row_value(const SettingRow *row, char *out, size_t outSize, bool shortForm)
{
	settings_value_name(row, settings_row_get(row, &g_settings), out, outSize, shortForm);
}

void settings_update_mode_row(void)
{
	int mode = math_clamp_int(g_settings.mode, 0, MODE_COUNT - 1);
	g_quickRows[QUICK_TILE_OPTION] = MODE_OPTION_ROWS[mode];
}

void settings_load_looks(void)
{
	int lookCount = look_load_all(LOOKS_DIR);
	SettingRow *lookTile = &g_quickRows[QUICK_TILE_LOOK];
	lookTile->max = lookCount > 0 ? lookCount - 1 : 0;
	lookTile->names = look_names();
	lookTile->shortNames = look_short_names();
}

void settings_refresh_look_tile(void)
{
	g_quickRows[QUICK_TILE_LOOK].max = look_count() - 1;
}
