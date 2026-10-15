#include "setting_actions.h"
#include <3ds.h>
#include "cam_settings.h"
#include "camera_control.h"
#include "device_status.h"
#include "gallery.h"
#include "key_repeat.h"
#include "look_editor.h"
#include "math_util.h"
#include "menus.h"
#include "metering.h"
#include "settings.h"
#include "shot_mode.h"
#include "toast.h"

void setting_actions_on_change(const SettingRow *row)
{
	if (row->offset == SETTING_OFFSET(cam.camera)) {
		// The self-facing camera behaves like a mirror.
		if (g_settings.cam.camera == CAM_INNER && g_settings.cam.flip == FLIP_NONE) {
			g_settings.cam.flip = FLIP_HORIZONTAL;
		} else if (g_settings.cam.camera != CAM_INNER && g_settings.cam.flip == FLIP_HORIZONTAL) {
			g_settings.cam.flip = FLIP_NONE;
		}
		// HDR and 3D need both outer cameras. The mode is kept and the inner camera takes plain photos.
		if (shot_mode_get(&g_settings) != math_clamp_int(g_settings.mode, 0, MODE_COUNT - 1)) {
			toast_show("%s: photos with the inner camera", MODE_SHORT_NAMES[(int)g_settings.mode]);
		}
	}
	if (settings_row_is_camera_setting(row)) {
		// The camera thread applies it, together with the stream the mode wants.
		camera_control_request(&g_settings.cam);
	} else if (row->offset == SETTING_OFFSET(mode)) {
		settings_update_mode_row();
		// MERGE, HDR and 3D want the dual raw stream, PHOTO the 60 fps view. Switch now, not at the shutter.
		camera_control_request_stream(true);
	}
	if (row->offset == SETTING_OFFSET(cam.format)) {
		device_status_refresh(true);
	}
}

// Enums and on / off rows wrap around like a dial and skip the values the shot mode cannot take now.
static int next_enabled_value(const SettingRow *row, int value, int direction)
{
	int valueCount = row->max - row->min + 1;
	int next = value;
	for (int i = 0; i < valueCount; i++) {
		next = row->min + ((next - row->min + direction + valueCount) % valueCount);
		if (!shot_mode_value_disabled_reason(row, next)) {
			break;
		}
	}
	return next;
}

void setting_actions_step_row(const SettingRow *row, int direction)
{
	// A disabled row says why, for example the option tile of a mode without an option.
	const char *disabledReason = shot_mode_row_disabled_reason(row);
	if (disabledReason) {
		toast_show("%s", disabledReason);
		return;
	}
	if (row->kind == ROW_ACTION) {
		return;
	}
	// An on / off row toggles once per press. A held key would toggle it on every repeat, and each toggle
	// saves the settings file and switches the camera stream.
	if (settings_row_is_switch(row)) {
		bool unusedQuit = false;
		if (!g_keyRepeated) {
			setting_actions_run(row->action, &unusedQuit);
		}
		return;
	}
	int value = settings_row_get(row, &g_settings);
	// Numbers and the exposure dials stop at their ends, so one step too many does not jump from 1 s to
	// 1/8000 s.
	bool stopsAtEnds = row->kind == ROW_INT || row->offset == SETTING_OFFSET(cam.shutter) ||
	                   row->offset == SETTING_OFFSET(cam.iso);
	int newValue;
	if (stopsAtEnds) {
		newValue = math_clamp_int(value + direction, row->min, row->max);
	} else {
		newValue = next_enabled_value(row, value, direction);
	}
	if (newValue == value) {
		return;
	}
	settings_row_set(row, &g_settings, newValue);
	setting_actions_on_change(row);
}

void setting_actions_swap_screens(void)
{
	g_screensSwapped = !g_screensSwapped;
	metering_clear_touch_point();
	settings_save();
	toast_show(g_screensSwapped ? "Viewfinder on the touch screen: tap to meter"
	                            : "Viewfinder on the top screen");
}

static void save_preset(void)
{
	settings_save_preset(g_settings.slot);
	toast_show("Saved to C%d", g_settings.slot + 1);
}

static void load_preset(void)
{
	if (!settings_load_preset(g_settings.slot)) {
		toast_show("C%d is empty", g_settings.slot + 1);
		return;
	}
	camera_control_request(&g_settings.cam);
	toast_show("Loaded C%d", g_settings.slot + 1);
}

static void reset_settings(void)
{
	settings_defaults(&g_settings);
	// The 60 fps view is an app switch outside Settings: presets leave it alone, a reset turns it on.
	g_fastLiveView = true;
	camera_control_request(&g_settings.cam);
	toast_show("All settings reset");
}

// The 60 fps view only works in PHOTO with the raw stream.
static void toggle_fast_live_view(void)
{
	const char *disabledReason = shot_mode_row_disabled_reason(&g_quickRows[QUICK_TILE_FAST_LIVE_VIEW]);
	if (disabledReason) {
		toast_show("%s", disabledReason);
		return;
	}
	g_fastLiveView = !g_fastLiveView;
	settings_save();
	// The camera thread switches the stream.
	camera_control_request(&g_settings.cam);
	toast_show(g_fastLiveView ? "Live view up to 60 fps (half the rows; photos stay full size)"
	                          : "Live view: full frames (30 fps)");
}

static void toggle_hot_pixel_fix(void)
{
	g_hotPixelFix = !g_hotPixelFix;
	settings_save();
	toast_show(g_hotPixelFix ? "Hot pixels are fixed in the raw (DNG) too"
	                         : "The raw (DNG) is left untouched");
}

void setting_actions_run(RowAction action, bool *quit)
{
	switch (action) {
	case ACTION_SAVE_PRESET:
		save_preset();
		break;
	case ACTION_LOAD_PRESET:
		load_preset();
		break;
	case ACTION_RESET:
		reset_settings();
		break;
	case ACTION_INFO:
		g_infoOpen = true;
		break;
	case ACTION_SWAP_SCREENS:
		setting_actions_swap_screens();
		g_menuOpen = false;
		break;
	case ACTION_GALLERY:
		g_menuOpen = false;
		gallery_open();
		break;
	case ACTION_LOOK_EDITOR:
		g_menuOpen = false;
		look_editor_open();
		break;
	case ACTION_FAST_LIVE_VIEW:
		toggle_fast_live_view();
		break;
	case ACTION_HOT_PIXEL_FIX:
		toggle_hot_pixel_fix();
		break;
	case ACTION_EXIT:
		*quit = true;
		break;
	default:
		break;
	}
}
