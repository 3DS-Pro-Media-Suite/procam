// What the shutter takes now, and which settings and values can be used, from the settings, raw access and
// the console model.
#pragma once
#include <3ds.h>
#include "settings.h"

// True when start-up got raw access to the sensors. Without it the camera streams YUV.
extern bool g_rawMode;

// The chosen mode, or Photo when the mode needs both outer cameras and the inner one is chosen.
int shot_mode_get(const Settings *settings);

// The mode a press takes now: also Photo for Merge and HDR while the stream is not raw.
int shot_mode_for_press(const Settings *settings);

// Whether both outer cameras should stream raw, ready for the shutter.
bool shot_mode_dual_wanted(const Settings *settings);

// Whether the 60 fps live view should run.
bool shot_mode_fast_wanted(const Settings *settings);

// Frames per camera for Merge and HDR, limited by the console's memory.
int shot_mode_merge_frames(const Settings *settings);

// What the JPG's width and height are divided by: 1, 2 or 4.
int shot_mode_photo_divisor(const Settings *settings);

// Why a setting cannot be used now (greyed out), or NULL.
const char *shot_mode_row_disabled_reason(const SettingRow *row);

// Why one value of a setting cannot be chosen now (dimmed, skipped by the dial), or NULL.
const char *shot_mode_value_disabled_reason(const SettingRow *row, int value);

// A note under the values of a row: why some of them are grey, or what they do in this mode. NULL: none.
const char *shot_mode_row_note(const SettingRow *row);
