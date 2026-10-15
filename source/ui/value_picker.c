#include "value_picker.h"
#include <3ds.h>
#include <citro2d.h>
#include <string.h>
#include "cam_settings.h"
#include "key_repeat.h"
#include "look_editor.h"
#include "setting_actions.h"
#include "settings.h"
#include "shot_mode.h"
#include "toast.h"
#include "touch.h"
#include "ui_layout.h"
#include "ui_panel.h"
#include "ui_shapes.h"
#include "ui_text.h"

// A few values take one column, more take two or three.
#define ONE_COLUMN_MAX_VALUES 6
#define TWO_COLUMNS_MAX_VALUES 14
#define LONG_NAMES_MAX_VALUES 8 // up to this many values, a long name keeps one column
#define LONG_NAME_LENGTH 28
#define PICKER_HEIGHT 204.0f
#define MAX_ROW_HEIGHT 30

#define FULL_TEXT_ROW_HEIGHT 22.0f // shorter rows shrink their text with them
#define BODY_TEXT_HEIGHT 16
#define VALUE_TEXT_LIFT 1 // the text sits a pixel above the centre
#define VALUE_TEXT_PADDING 22 // both sides together; the text stays clear of the default dot
#define VALUE_NAME_SIZE 48
#define DEFAULT_DOT_X 7 // a dot marks the default value
#define DEFAULT_DOT_RADIUS 2

#define FORMAT_HELP_X 14
#define FORMAT_HELP_Y 108
#define FORMAT_HELP_LINE_HEIGHT 14
#define FORMAT_HELP_LINES 8
#define FORMAT_HELP_RGB565_LINE 3
#define FORMAT_HELP_RAW_LINE 5

#define LIST_REPEAT_FRAMES 4 // key repeat period in frames

static const SettingRow *g_pickerRow; // the setting the open picker shows, NULL while it is closed
static int g_pickerSelection; // the D-pad cursor, as an index from the row's min

bool value_picker_active(void)
{
	return g_pickerRow != NULL;
}

void value_picker_close(void)
{
	g_pickerRow = NULL;
}

int value_picker_count(const SettingRow *row)
{
	return row->max - row->min + 1;
}

static int picker_columns(const SettingRow *row)
{
	int count = value_picker_count(row);
	if (count <= ONE_COLUMN_MAX_VALUES) {
		return 1;
	}
	// Long names keep one column: in half the width they would shrink to half size.
	if (count <= LONG_NAMES_MAX_VALUES && row->names) {
		for (int i = 0; i < count; i++) {
			if (strlen(row->names[i]) > LONG_NAME_LENGTH) {
				return 1;
			}
		}
	}
	return count <= TWO_COLUMNS_MAX_VALUES ? 2 : 3;
}

void value_picker_rect(const SettingRow *row, int index, float *x, float *y, float *width, float *height)
{
	int count = value_picker_count(row);
	int columns = picker_columns(row);
	int rows = (count + columns - 1) / columns;
	float rowHeight = (PICKER_HEIGHT - (rows - 1) * TILE_GAP) / rows;
	if (rowHeight > MAX_ROW_HEIGHT) {
		rowHeight = MAX_ROW_HEIGHT;
	}
	*width = ((float)PANEL_ROW_WIDTH - (columns - 1) * TILE_GAP) / columns;
	*height = rowHeight;
	// Values run down the first column, then the next, so they stay in reading order.
	int column = index / rows;
	int rowIndex = index % rows;
	*x = PANEL_ROW_X + column * (*width + TILE_GAP);
	*y = PANEL_CONTENT_Y + rowIndex * (rowHeight + TILE_GAP);
}

void value_picker_open(const SettingRow *row)
{
	const char *disabledReason = shot_mode_row_disabled_reason(row);
	if (disabledReason) {
		toast_show("%s", disabledReason);
		return;
	}
	if (row->kind == ROW_ACTION) {
		return;
	}
	g_pickerRow = row;
	g_pickerSelection = settings_row_get(row, &g_settings) - row->min;
}

static void draw_title_bar(const SettingRow *row)
{
	const char *note = row == &g_quickRows[QUICK_TILE_LOOK] ? "X edit   B back" : "B back";
	ui_panel_draw_header('P', row->label, note, COLOR_FAINT);
}

static u32 value_text_color(bool disabled, bool current)
{
	if (disabled) {
		return COLOR_FAINT;
	}
	if (current) {
		return COLOR_ACCENT;
	}
	return COLOR_INK;
}

static void draw_value_tile(const SettingRow *row, int index, int currentIndex)
{
	float x;
	float y;
	float width;
	float height;
	value_picker_rect(row, index, &x, &y, &width, &height);
	bool current = index == currentIndex;
	bool selected = index == g_pickerSelection;
	bool disabled = shot_mode_value_disabled_reason(row, row->min + index) != NULL;
	if (selected) {
		ui_shapes_round_rect(x - SELECTION_OUTLINE, y - SELECTION_OUTLINE, width + 2 * SELECTION_OUTLINE,
		                     height + 2 * SELECTION_OUTLINE, SELECTION_RADIUS, COLOR_ACCENT, DEPTH_PANEL);
	}
	u32 tileColor = current ? COLOR_TILE_SELECTED : COLOR_TILE;
	ui_shapes_round_rect(x, y, width, height, TILE_RADIUS, tileColor, DEPTH_TILE);
	char name[VALUE_NAME_SIZE];
	settings_value_name(row, row->min + index, name, sizeof name, false);
	float textScale = height < FULL_TEXT_ROW_HEIGHT ? height / FULL_TEXT_ROW_HEIGHT : 1.0f;
	float textY = y + (height - BODY_TEXT_HEIGHT * textScale) / 2 - VALUE_TEXT_LIFT;
	u32 textColor = value_text_color(disabled, current);
	ui_text_draw_string(g_fontBody, x + width / 2, textY, textScale, textColor, TEXT_ALIGN_CENTER,
	                    width - VALUE_TEXT_PADDING, name);
	if (row->min + index == row->defaultValue) {
		ui_shapes_circle(x + DEFAULT_DOT_X, y + height / 2, DEFAULT_DOT_RADIUS, COLOR_FAINT, DEPTH_CONTROL);
	}
}

static void draw_format_help(void)
{
	static const char *const lines[FORMAT_HELP_LINES] = {
	        "YUV 4:2:2: 8-bit luma (256 levels) for every pixel;",
	        "colour is shared by each pair of pixels. The finest",
	        "tonal detail the camera puts out.",
	        "RGB565: 5-bit red and blue, 6-bit green (32 / 64 levels):",
	        "banding in smooth gradients and in the shadows.",
	        "Only without raw access (the camera's own YUV stream):",
	        "with it, every photo is the sensor's raw frame (DNG).",
	        "3D pairs on the YUV stream are YUV too.",
	};
	for (int i = 0; i < FORMAT_HELP_LINES; i++) {
		bool muted = i == FORMAT_HELP_RGB565_LINE || i == FORMAT_HELP_RAW_LINE;
		u32 color = muted ? COLOR_MUTED : COLOR_INK_DIM;
		int lineY = FORMAT_HELP_Y + i * FORMAT_HELP_LINE_HEIGHT;
		ui_text_draw(g_fontSmall, FORMAT_HELP_X, lineY, color, TEXT_ALIGN_LEFT, "%s", lines[i]);
	}
}

// Notes under the values: why some of them are grey, and what the formats carry.
static void draw_footer_notes(const SettingRow *row)
{
	const char *note = shot_mode_row_note(row);
	if (note) {
		ui_text_draw(g_fontSmall, BOTTOM_CENTER_X, PANEL_FOOTER_TEXT_Y, COLOR_MUTED, TEXT_ALIGN_CENTER, "%s",
		             note);
	}
	if (row->offset == SETTING_OFFSET(cam.format)) {
		draw_format_help();
	}
}

void value_picker_draw(void)
{
	const SettingRow *row = g_pickerRow;
	draw_title_bar(row);
	int currentIndex = settings_row_get(row, &g_settings) - row->min;
	for (int i = 0; i < value_picker_count(row); i++) {
		draw_value_tile(row, i, currentIndex);
	}
	draw_footer_notes(row);
}

// True when index is a value of the row other than its current one.
static bool is_new_value(const SettingRow *row, int index)
{
	return index >= 0 && index < value_picker_count(row) &&
	       settings_row_get(row, &g_settings) != row->min + index;
}

void value_picker_choose(int index)
{
	const SettingRow *row = g_pickerRow;
	g_pickerRow = NULL;
	// A two-way switch changes through its action.
	if (settings_row_is_switch(row)) {
		bool unused = false;
		if (is_new_value(row, index)) {
			setting_actions_run(row->action, &unused);
		}
		return;
	}
	if (!is_new_value(row, index)) {
		return;
	}
	const char *disabledReason = shot_mode_value_disabled_reason(row, row->min + index);
	if (disabledReason) {
		toast_show("%s", disabledReason);
		g_pickerRow = row; // the picker stays open
		return;
	}
	settings_row_set(row, &g_settings, row->min + index);
	setting_actions_on_change(row);
}

static int touched_value(const SettingRow *row, int valueCount, touchPosition touch)
{
	for (int i = 0; i < valueCount; i++) {
		float x;
		float y;
		float width;
		float height;
		value_picker_rect(row, i, &x, &y, &width, &height);
		if (touch_in_rect(touch, x, y, width, height)) {
			return i;
		}
	}
	return -1;
}

void value_picker_input(u32 down, u32 held, touchPosition touch)
{
	const SettingRow *row = g_pickerRow;
	int valueCount = value_picker_count(row);
	if (down & (KEY_B | KEY_START)) {
		g_pickerRow = NULL;
		return;
	}
	// In the look picker, X edits the highlighted look.
	if ((down & KEY_X) && row == &g_quickRows[QUICK_TILE_LOOK]) {
		value_picker_choose(g_pickerSelection);
		look_editor_open();
		return;
	}
	if (key_repeat_dpad(held, KEY_DDOWN, LIST_REPEAT_FRAMES)) {
		g_pickerSelection = (g_pickerSelection + 1) % valueCount;
	}
	if (key_repeat_dpad(held, KEY_DUP, LIST_REPEAT_FRAMES)) {
		g_pickerSelection = (g_pickerSelection + valueCount - 1) % valueCount;
	}
	if (down & KEY_A) {
		value_picker_choose(g_pickerSelection);
		return;
	}
	if (!(down & KEY_TOUCH)) {
		return;
	}
	int touchedIndex = touched_value(row, valueCount, touch);
	if (touchedIndex >= 0) {
		value_picker_choose(touchedIndex);
	} else {
		// A tap outside the values closes the picker.
		g_pickerRow = NULL;
	}
}
