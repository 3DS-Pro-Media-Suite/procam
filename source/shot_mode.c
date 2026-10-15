#include "shot_mode.h"
#include <3ds.h>
#include "cam_settings.h"
#include "cam_stream.h"
#include "math_util.h"
#include "settings.h"

// A number as text, for string constants.
#define NUMBER_TEXT(number) NUMBER_TEXT_OF(number)
#define NUMBER_TEXT_OF(number) #number

// An Old 3DS has much less heap than a New 3DS, so its bursts stay small.
#define OLD_3DS_MAX_MERGE_FRAMES 6
#define OLD_3DS_FRAMES_NOTE \
	"Old 3DS: " NUMBER_TEXT(OLD_3DS_MAX_MERGE_FRAMES) " frames per camera at most (memory)"

// How much each Settings.photoSize divides the width and height.
#define HALF_SIZE_DIVISOR 2
#define QUARTER_SIZE_DIVISOR 4

// The camera's own image controls work on its processor, and a raw frame is taken before it.
static const size_t YUV_ONLY_SETTING_OFFSETS[] = {
        SETTING_OFFSET(cam.whiteBalance), SETTING_OFFSET(cam.awbLock),     SETTING_OFFSET(cam.awbWindow),
        SETTING_OFFSET(cam.sharpness),    SETTING_OFFSET(cam.noiseFilter), SETTING_OFFSET(cam.contrast),
        SETTING_OFFSET(cam.photoMode),    SETTING_OFFSET(cam.effect),      SETTING_OFFSET(cam.lensCorrection),
        SETTING_OFFSET(cam.frameRate),    SETTING_OFFSET(cam.format)};

bool g_rawMode;

static bool needs_both_outer_cameras(int mode)
{
	return mode == MODE_HDR || mode == MODE_3D;
}

static bool merges_raw_frames(int mode)
{
	return mode == MODE_MERGE || mode == MODE_HDR;
}

static bool uses_dual_stream(int mode)
{
	return mode == MODE_MERGE || mode == MODE_HDR || mode == MODE_3D;
}

// The chosen mode stays in the settings, so it comes back when an outer camera is chosen again.
int shot_mode_get(const Settings *settings)
{
	int mode = math_clamp_int(settings->mode, 0, MODE_COUNT - 1);
	return (settings->cam.camera == CAM_INNER && needs_both_outer_cameras(mode)) ? MODE_PHOTO : mode;
}

int shot_mode_for_press(const Settings *settings)
{
	int mode = shot_mode_get(settings);
	return (!cam_stream_raw_active() && merges_raw_frames(mode)) ? MODE_PHOTO : mode;
}

static const char *mode_unavailable_reason(const Settings *settings, int mode)
{
	if (!g_rawMode && merges_raw_frames(mode)) {
		if (mode == MODE_HDR) {
			return "HDR merges raw frames: no raw access (a photo instead)";
		}
		return "Merge merges raw frames: no raw access (a photo instead)";
	}
	if (settings->cam.camera != CAM_INNER) {
		return NULL;
	}
	if (mode == MODE_HDR) {
		return "HDR uses both outer cameras: not with the inner one";
	}
	if (mode == MODE_3D) {
		return "3D uses both outer cameras: not with the inner one";
	}
	return NULL;
}

// The dual stream keeps both outer sensors running raw at full frames, so the shutter needs no sensor set-up.
// It costs power and heat, so it runs only in the modes that use it.
bool shot_mode_dual_wanted(const Settings *settings)
{
	int mode = shot_mode_get(settings);
	return g_rawMode && settings->cam.camera != CAM_INNER && uses_dual_stream(mode);
}

// The other modes take full frames straight from the stream.
bool shot_mode_fast_wanted(const Settings *settings)
{
	return g_fastLiveView && shot_mode_get(settings) == MODE_PHOTO;
}

int shot_mode_merge_frames(const Settings *settings)
{
	int frames = settings_burst_frames_per_camera(settings->burstFrames);
	if (!g_isNew3ds && frames > OLD_3DS_MAX_MERGE_FRAMES) {
		return OLD_3DS_MAX_MERGE_FRAMES;
	}
	return frames;
}

int shot_mode_photo_divisor(const Settings *settings)
{
	if (settings->photoSize == PHOTO_SIZE_QUARTER) {
		return QUARTER_SIZE_DIVISOR;
	}
	return settings->photoSize == PHOTO_SIZE_HALF ? HALF_SIZE_DIVISOR : 1;
}

// A row that holds a Settings value, not an action or a switch.
static bool is_value_row(const SettingRow *row)
{
	return row->kind != ROW_ACTION && !settings_row_is_switch(row);
}

static bool is_yuv_only_row(const SettingRow *row)
{
	if (!is_value_row(row)) {
		return false;
	}
	for (int i = 0; i < ARRAY_COUNT(YUV_ONLY_SETTING_OFFSETS); i++) {
		if (row->offset == YUV_ONLY_SETTING_OFFSETS[i]) {
			return true;
		}
	}
	return false;
}

// Exposure controls that only the raw stream's own exposure loop uses.
static bool is_raw_exposure_row(const SettingRow *row)
{
	return row->offset == SETTING_OFFSET(cam.exposureMode) || row->offset == SETTING_OFFSET(cam.shutter) ||
	       row->offset == SETTING_OFFSET(cam.iso);
}

static bool is_look_row(const SettingRow *row)
{
	return row->offset == SETTING_OFFSET(lookIndex) || row->action == ACTION_LOOK_EDITOR;
}

const char *shot_mode_row_disabled_reason(const SettingRow *row)
{
	int mode = math_clamp_int(g_settings.mode, 0, MODE_COUNT - 1);
	bool rawActive = cam_stream_raw_active();
	bool isValueRow = is_value_row(row);

	// The option tile changes with the mode.
	if (row == &g_quickRows[QUICK_TILE_OPTION]) {
		const char *modeReason = mode_unavailable_reason(&g_settings, mode);
		if (modeReason) {
			return modeReason;
		}
		if (row->kind == ROW_ACTION) {
			return mode == MODE_3D ? "3D has no option of its own" : "Photo has no option of its own";
		}
	}
	if (rawActive && is_yuv_only_row(row)) {
		return "YUV mode only: raw skips the camera's processing (white balance: auto)";
	}
	if (isValueRow && is_raw_exposure_row(row) && !rawActive) {
		return "Raw only: the camera's YUV stream keeps its own exposure (EV still works)";
	}
	if (isValueRow && row->offset == SETTING_OFFSET(cam.shutter) &&
	    g_settings.cam.exposureMode == CAM_EXPOSURE_AUTO) {
		return "Auto exposure sets the time (S or M: yours)";
	}
	if (isValueRow && row->offset == SETTING_OFFSET(cam.iso) &&
	    g_settings.cam.exposureMode != CAM_EXPOSURE_MANUAL) {
		return "The exposure loop sets the gain (M: yours)";
	}
	if (isValueRow && row->offset == SETTING_OFFSET(cam.exposure) && rawActive &&
	    g_settings.cam.exposureMode == CAM_EXPOSURE_MANUAL) {
		return "Manual: no exposure bias";
	}
	if (isValueRow && row->offset == SETTING_OFFSET(photoFormat) && !rawActive) {
		return "Raw only: the camera's YUV stream saves PNG + YUV";
	}
	if (isValueRow && row->offset == SETTING_OFFSET(photoSize) && rawActive &&
	    g_settings.photoFormat == PHOTO_FORMAT_RAW_ONLY) {
		return "RAW only: no JPG, so no photo size (the DNG is the full frame)";
	}
	if (is_look_row(row) && g_settings.photoFormat == PHOTO_FORMAT_RAW_ONLY && rawActive) {
		return "RAW only: no JPG, so no look (the DNG is untouched)";
	}
	if (row->action == ACTION_FAST_LIVE_VIEW && !g_rawMode) {
		return "60 fps needs the raw stream (the camera's own YUV runs at 30 fps)";
	}
	if (row->action == ACTION_FAST_LIVE_VIEW && shot_mode_get(&g_settings) != MODE_PHOTO) {
		return "60 fps is for Photo mode: the others need full frames";
	}
	return NULL;
}

static bool too_many_frames_for_old_3ds(const SettingRow *row, int value)
{
	if (row->offset == SETTING_OFFSET(burstFrames)) {
		return settings_burst_frames_per_camera(value) > OLD_3DS_MAX_MERGE_FRAMES;
	}
	if (settings_row_is_hdr(row)) {
		int burstIndex = settings_hdr_option_burst_index(value);
		return settings_burst_frames_per_camera(burstIndex) > OLD_3DS_MAX_MERGE_FRAMES;
	}
	return false;
}

const char *shot_mode_value_disabled_reason(const SettingRow *row, int value)
{
	if (row->kind != ROW_ENUM) {
		return NULL;
	}
	if (row->offset == SETTING_OFFSET(mode)) {
		return mode_unavailable_reason(&g_settings, value);
	}
	if (!g_isNew3ds && too_many_frames_for_old_3ds(row, value)) {
		return OLD_3DS_FRAMES_NOTE;
	}
	return NULL;
}

const char *shot_mode_row_note(const SettingRow *row)
{
	bool modeRow = row->offset == SETTING_OFFSET(mode);
	if (modeRow && !g_rawMode) {
		return "Merge and HDR merge raw frames: no raw access here";
	}
	if (modeRow && g_settings.cam.camera == CAM_INNER) {
		return "HDR and 3D use both outer cameras: not with the inner one";
	}
	if (row->offset == SETTING_OFFSET(photoFormat) && shot_mode_get(&g_settings) == MODE_3D) {
		return "3D: a raw pair always keeps both DNGs (JPG only too)";
	}
	// The HDR row has no room for this note: its twelve values fill the page.
	if (!g_isNew3ds && row->offset == SETTING_OFFSET(burstFrames)) {
		return OLD_3DS_FRAMES_NOTE;
	}
	return NULL;
}
