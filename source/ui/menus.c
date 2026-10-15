#include "menus.h"
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include "cam_settings.h"
#include "cam_stream.h"
#include "camera_control.h"
#include "gallery.h"
#include "gallery_panel.h"
#include "key_repeat.h"
#include "look_editor.h"
#include "look_editor_draw.h"
#include "math_util.h"
#include "preview.h"
#include "saver.h"
#include "self_timer.h"
#include "setting_actions.h"
#include "settings.h"
#include "shot_mode.h"
#include "touch.h"
#include "ui_layout.h"
#include "ui_panel.h"
#include "ui_shapes.h"
#include "ui_text.h"
#include "value_picker.h"

#define QUICK_GRID_Y 25
#define HEADER_NOTE_SIZE 16
#define LIST_REPEAT_FRAMES 4 // key repeat period in frames

#define GALLERY_BUTTON_RADIUS 4
#define GALLERY_BUTTON_TEXT_Y (GALLERY_BUTTON_Y + 3)

// The shutter button: a ring, a dark gap and the centre, each drawn on top of the last. A tap counts within
// SHUTTER_TOUCH_RADIUS.
#define SHUTTER_TOUCH_RADIUS 20
#define SHUTTER_RING_RADIUS 16
#define SHUTTER_GAP_RADIUS 14
#define SHUTTER_CENTER_RADIUS 11.5f
#define DEPTH_CONTROL_INNER 0.31f
#define DEPTH_CONTROL_CENTER 0.32f

// A quick menu tile: the tag at the top left, the value centred below it and a dot for a changed setting.
#define TILE_TAG_X 7
#define TILE_TAG_Y 4
#define TILE_VALUE_Y 17
#define TILE_VALUE_MARGIN 5
#define TILE_VALUE_SIZE 40
#define TILE_LOCKED_VALUE_SIZE 48
#define CHANGED_DOT_INSET 8
#define CHANGED_DOT_Y 9
#define CHANGED_DOT_RADIUS 2.2f

// The value bar under the tiles.
#define VALUE_BAR_LINE_Y 200
#define VALUE_BAR_LABEL_Y 204
#define VALUE_BAR_VALUE_Y 217
#define VALUE_BAR_VALUE_WIDTH 186
#define VALUE_BAR_TEXT_SIZE 48
#define ARROW_POINTER_SIZE 5

#define INFO_FIRST_LINE_Y 30
#define INFO_LINE_HEIGHT 18
#define INFO_CLOSE_HINT_Y 220

// The setup menu: section tabs under the title bar, then the rows.
#define MENU_TAB_TEXT_Y 27
#define MENU_TAB_MARK_Y 41
#define MENU_TAB_MARK_WIDTH 36
#define MENU_TAB_MARK_HEIGHT 2
#define MENU_TAB_MARK_RADIUS 1
#define MENU_ROW_BOX_HEIGHT 23
#define MENU_ROW_MIDDLE_Y (MENU_ROW_BOX_HEIGHT / 2.0f)
#define MENU_ROW_TEXT_Y 3
#define MENU_ROW_MARK_Y 5
#define MENU_ROW_MARK_HEIGHT 13
#define MENU_ROW_VALUE_SIZE 40
#define MENU_ACTION_POINTER_X 302
#define MENU_ACTION_POINTER_SIZE 4
#define MENU_SELECTED_VALUE_X 296 // right edge of a selected value, left of its arrow
#define MENU_NEXT_POINTER_X 303
#define MENU_PREVIOUS_POINTER_GAP 8
#define MENU_VALUE_POINTER_SIZE 3.5f
#define MENU_VERSION_Y 227
#define MENU_ROWS_BOTTOM (MENU_ROW_Y + MENU_VISIBLE_ROWS * MENU_ROW_HEIGHT)

// Marks for more rows above or below the visible ones.
#define SCROLL_UP_MARK_Y 47.6f
#define SCROLL_DOWN_MARK_BASE_Y 201.5f

int g_quickSelection;
bool g_menuOpen;
bool g_infoOpen;
static int g_menuPage;
static int g_menuSelection[SETTINGS_PAGE_COUNT];

// First row shown on each page. A page shows MENU_VISIBLE_ROWS rows and scrolls with the selection.
static int g_menuTopRow[SETTINGS_PAGE_COUNT];

// Scrolls the current page so that its selected row is visible.
static void keep_selection_visible(void)
{
	const SettingPage *page = &SETTINGS_PAGES[g_menuPage];
	int *topRow = &g_menuTopRow[g_menuPage];
	int selection = g_menuSelection[g_menuPage];
	if (selection < *topRow) {
		*topRow = selection;
	}
	if (selection >= *topRow + MENU_VISIBLE_ROWS) {
		*topRow = selection - MENU_VISIBLE_ROWS + 1;
	}
	int lastTopRow = page->count > MENU_VISIBLE_ROWS ? page->count - MENU_VISIBLE_ROWS : 0;
	*topRow = math_clamp_int(*topRow, 0, lastTopRow);
}

bool menus_touch_on_shutter(touchPosition touch)
{
	int dx = touch.px - SHUTTER_BUTTON_X;
	int dy = touch.py - SHUTTER_BUTTON_Y;
	return dx * dx + dy * dy < SHUTTER_TOUCH_RADIUS * SHUTTER_TOUCH_RADIUS;
}

void menus_quick_tile_rect(int tile, float *x, float *y)
{
	*x = PANEL_ROW_X + (tile % QUICK_GRID_COLUMNS) * (QUICK_TILE_WIDTH + TILE_GAP);
	*y = QUICK_GRID_Y + (tile / QUICK_GRID_COLUMNS) * (QUICK_TILE_HEIGHT + TILE_GAP);
}

static void draw_divider(float y)
{
	C2D_DrawRectSolid(0, y, DEPTH_PANEL, BOTTOM_SCREEN_WIDTH, 1, COLOR_LINE);
}

// The title bar with the frame rate and the gallery button.
static void draw_header(const char *title)
{
	char fpsNote[HEADER_NOTE_SIZE];
	snprintf(fpsNote, sizeof fpsNote, "%.1f fps", cam_stream_fps());
	const char *note = camera_control_ok() ? fpsNote : NULL;
	ui_panel_draw_header(g_menuOpen ? 'M' : 'Q', title, note, COLOR_FAINT);
	if (!gallery_is_open()) {
		ui_shapes_round_rect(GALLERY_BUTTON_X, GALLERY_BUTTON_Y, GALLERY_BUTTON_WIDTH, GALLERY_BUTTON_HEIGHT,
		                     GALLERY_BUTTON_RADIUS, COLOR_TILE_SELECTED, DEPTH_PANEL);
		ui_text_draw(g_fontSmall, GALLERY_BUTTON_X + GALLERY_BUTTON_WIDTH / 2, GALLERY_BUTTON_TEXT_Y,
		             COLOR_ACCENT, TEXT_ALIGN_CENTER, "GALLERY");
	}
}

// Red while the self-timer counts down.
static void draw_shutter_button(void)
{
	bool timerCounting = self_timer_counting();
	u32 ringColor = timerCounting ? COLOR_RED : COLOR_INK;
	ui_shapes_circle(SHUTTER_BUTTON_X, SHUTTER_BUTTON_Y, SHUTTER_RING_RADIUS, ringColor, DEPTH_CONTROL);
	ui_shapes_circle(SHUTTER_BUTTON_X, SHUTTER_BUTTON_Y, SHUTTER_GAP_RADIUS, COLOR_BLACK,
	                 DEPTH_CONTROL_INNER);
	u32 centerColor = timerCounting ? COLOR_RED : COLOR_ACCENT;
	ui_shapes_circle(SHUTTER_BUTTON_X, SHUTTER_BUTTON_Y, SHUTTER_CENTER_RADIUS, centerColor,
	                 DEPTH_CONTROL_CENTER);
}

static u32 tile_tag_color(bool rowDisabled, bool selected)
{
	if (rowDisabled) {
		return COLOR_FAINT;
	}
	if (selected) {
		return COLOR_ACCENT;
	}
	return COLOR_MUTED;
}

static u32 tile_value_color(bool valueDisabled, bool exposureLocked, bool selected)
{
	if (valueDisabled) {
		return COLOR_FAINT;
	}
	if (exposureLocked) {
		return COLOR_ACCENT;
	}
	if (selected) {
		return COLOR_INK;
	}
	return COLOR_INK_SOFT;
}

static void draw_quick_tile(int tile)
{
	const SettingRow *row = &g_quickRows[tile];
	float x;
	float y;
	menus_quick_tile_rect(tile, &x, &y);
	bool selected = tile == g_quickSelection;
	// HDR and 3D need the outer cameras, RAW only has no look and 60 fps is PHOTO's alone.
	bool rowDisabled = shot_mode_row_disabled_reason(row) != NULL;
	// The MODE tile also dims a mode the inner camera cannot take, or one whose dual stream is down.
	bool valueDisabled =
	        rowDisabled || shot_mode_value_disabled_reason(row, settings_row_get(row, &g_settings)) != NULL;
	valueDisabled |= tile == QUICK_TILE_MODE && camera_control_dual_lost(&g_settings);
	if (selected) {
		ui_shapes_round_rect(
		        x - SELECTION_OUTLINE, y - SELECTION_OUTLINE, QUICK_TILE_WIDTH + 2 * SELECTION_OUTLINE,
		        QUICK_TILE_HEIGHT + 2 * SELECTION_OUTLINE, SELECTION_RADIUS, COLOR_ACCENT, DEPTH_PANEL);
	}
	u32 tileColor = selected ? COLOR_TILE_SELECTED : COLOR_TILE;
	ui_shapes_round_rect(x, y, QUICK_TILE_WIDTH, QUICK_TILE_HEIGHT, TILE_RADIUS, tileColor, DEPTH_TILE);
	ui_text_draw(g_fontSmall, x + TILE_TAG_X, y + TILE_TAG_Y, tile_tag_color(rowDisabled, selected),
	             TEXT_ALIGN_LEFT, "%s", row->tag);
	if (!rowDisabled && row->kind != ROW_ACTION && settings_row_get(row, &g_settings) != row->defaultValue) {
		ui_shapes_circle(x + QUICK_TILE_WIDTH - CHANGED_DOT_INSET, y + CHANGED_DOT_Y, CHANGED_DOT_RADIUS,
		                 COLOR_ACCENT, DEPTH_CONTROL);
	}
	char value[TILE_VALUE_SIZE];
	settings_row_value(row, value, sizeof value, true);
	if (row->kind == ROW_ACTION) {
		// A mode without an option of its own.
		snprintf(value, sizeof value, "none");
	}
	// The exposure lock shows on the exposure mode's tile.
	bool exposureLocked = tile == QUICK_TILE_EXPOSURE_MODE && !g_settings.cam.autoExposure;
	char lockedValue[TILE_LOCKED_VALUE_SIZE];
	if (exposureLocked) {
		snprintf(lockedValue, sizeof lockedValue, "%s AE-L", value);
	}
	u32 valueColor = tile_value_color(valueDisabled, exposureLocked, selected);
	ui_text_draw_string(g_fontBody, x + QUICK_TILE_WIDTH / 2, y + TILE_VALUE_Y, 1.0f, valueColor,
	                    TEXT_ALIGN_CENTER, QUICK_TILE_WIDTH - 2 * TILE_VALUE_MARGIN,
	                    exposureLocked ? lockedValue : value);
}

static void draw_arrow_button(float x, int direction)
{
	ui_shapes_round_rect(x, ARROW_Y, ARROW_WIDTH, ARROW_HEIGHT, TILE_RADIUS, COLOR_TILE, DEPTH_CONTROL);
	ui_shapes_pointer(x + ARROW_WIDTH / 2, ARROW_Y + ARROW_HEIGHT / 2, ARROW_POINTER_SIZE, direction,
	                  COLOR_INK, DEPTH_CONTROL_INNER);
}

// The selected setting in full, with the arrows that step it.
static void draw_value_bar(void)
{
	draw_divider(VALUE_BAR_LINE_Y);
	const SettingRow *row = &g_quickRows[g_quickSelection];
	char value[VALUE_BAR_TEXT_SIZE];
	settings_row_value(row, value, sizeof value, false);
	bool dualLost = g_quickSelection == QUICK_TILE_MODE && camera_control_dual_lost(&g_settings);
	const char *label = dualLost ? "Mode (one camera: change it to retry)" : row->label;
	ui_text_draw(g_fontSmall, PANEL_TEXT_X, VALUE_BAR_LABEL_Y, COLOR_MUTED, TEXT_ALIGN_LEFT, "%s", label);
	bool valueDisabled = shot_mode_value_disabled_reason(row, settings_row_get(row, &g_settings)) || dualLost;
	u32 valueColor = valueDisabled ? COLOR_MUTED : COLOR_INK;
	ui_text_draw_string(g_fontBody, PANEL_TEXT_X, VALUE_BAR_VALUE_Y, 1.0f, valueColor, TEXT_ALIGN_LEFT,
	                    VALUE_BAR_VALUE_WIDTH, value);
	draw_arrow_button(ARROW_LEFT_X, POINTER_DIRECTION_LEFT);
	draw_arrow_button(ARROW_RIGHT_X, POINTER_DIRECTION_RIGHT);
}

static void draw_quick_menu(void)
{
	draw_header("QUICK MENU");
	for (int i = 0; i < QUICK_TILE_COUNT; i++) {
		draw_quick_tile(i);
	}
	draw_value_bar();
	draw_shutter_button();
}

static const char *stream_format_name(void)
{
	if (cam_stream_raw_active()) {
		return "RAW10";
	}
	if (g_settings.cam.format != CAM_FORMAT_RGB565) {
		return "YUV";
	}
	return "RGB565";
}

static void draw_camera_info(void)
{
	draw_header("CAMERA INFORMATION");
	CamStreamInfo info;
	cam_stream_info(&info);
	float y = INFO_FIRST_LINE_Y;
	ui_text_draw(g_fontSmall, PANEL_TEXT_X, y, COLOR_INK, TEXT_ALIGN_LEFT, "%s camera, active mask 0x%lX",
	             CAM_CAMERA_NAMES[(int)g_settings.cam.camera], info.activated);
	y += INFO_LINE_HEIGHT;
	ui_text_draw(g_fontSmall, PANEL_TEXT_X, y, COLOR_INK, TEXT_ALIGN_LEFT,
	             "Stream %d" UTF8_MULTIPLICATION_SIGN "%d %s, %.1f fps, %lu frames", g_previewWidth,
	             g_previewHeight, stream_format_name(), info.fps, info.frames);
	y += INFO_LINE_HEIGHT;
	ui_text_draw(g_fontSmall, PANEL_TEXT_X, y, COLOR_INK, TEXT_ALIGN_LEFT,
	             "Recovered stalls / buffer errors: %lu", info.restarts);
	y += INFO_LINE_HEIGHT;
	if (R_FAILED(info.lastError)) {
		const char *failedCall = info.lastErrorCall ? info.lastErrorCall : "?";
		// Only the function's name: the recorded call text has its arguments too.
		int callNameLength = (int)strcspn(failedCall, "(");
		ui_text_draw(g_fontSmall, PANEL_TEXT_X, y, COLOR_RED, TEXT_ALIGN_LEFT, "Last error 0x%08lX in %.*s",
		             info.lastError, callNameLength, failedCall);
	} else {
		ui_text_draw(g_fontSmall, PANEL_TEXT_X, y, COLOR_INK, TEXT_ALIGN_LEFT,
		             "Last configuration: all calls succeeded");
	}
	y += INFO_LINE_HEIGHT;
	if (info.haveCalibration) {
		ui_text_draw(g_fontSmall, PANEL_TEXT_X, y, COLOR_INK, TEXT_ALIGN_LEFT,
		             "Stereo base %.1f, view L %.1f R %.1f", info.calibration.distanceCameras,
		             info.calibration.angleOfViewLeft, info.calibration.angleOfViewRight);
	}
	y += INFO_LINE_HEIGHT;
	ui_text_draw(g_fontSmall, PANEL_TEXT_X, y, COLOR_INK, TEXT_ALIGN_LEFT,
	             "Photos this session: %d, card space for %d", g_shotsSaved, saver_shots_left());
	y += INFO_LINE_HEIGHT;
	ui_text_draw(g_fontSmall, PANEL_TEXT_X, y, COLOR_MUTED, TEXT_ALIGN_LEFT,
	             "Photos: " PHOTO_DIR " (DNG, JPG, TXT)");
	y += INFO_LINE_HEIGHT;
	ui_text_draw(g_fontSmall, PANEL_TEXT_X, y, COLOR_MUTED, TEXT_ALIGN_LEFT, "Version " APP_VERSION);
	ui_text_draw(g_fontSmall, BOTTOM_CENTER_X, INFO_CLOSE_HINT_Y, COLOR_FAINT, TEXT_ALIGN_CENTER,
	             "B to close");
}

// One tab per settings page, in the second header row.
static void draw_menu_tabs(void)
{
	for (int i = 0; i < SETTINGS_PAGE_COUNT; i++) {
		float centerX = MENU_TAB_WIDTH / 2 + i * MENU_TAB_WIDTH;
		bool current = i == g_menuPage;
		u32 textColor = current ? COLOR_INK : COLOR_MUTED;
		ui_text_draw(g_fontSmall, centerX, MENU_TAB_TEXT_Y, textColor, TEXT_ALIGN_CENTER, "%s",
		             SETTINGS_PAGES[i].name);
		if (current) {
			ui_shapes_round_rect(centerX - MENU_TAB_MARK_WIDTH / 2, MENU_TAB_MARK_Y, MENU_TAB_MARK_WIDTH,
			                     MENU_TAB_MARK_HEIGHT, MENU_TAB_MARK_RADIUS, COLOR_ACCENT, DEPTH_CONTROL);
		}
	}
	draw_divider(PANEL_SUBHEADER_LINE_Y);
}

static u32 menu_label_color(bool rowDisabled, bool selected)
{
	if (rowDisabled) {
		return COLOR_FAINT;
	}
	if (selected) {
		return COLOR_INK;
	}
	return COLOR_INK_SOFT;
}

static void draw_menu_row(const SettingRow *row, float y, bool selected)
{
	if (selected) {
		ui_shapes_round_rect(PANEL_ROW_X, y, PANEL_ROW_WIDTH, MENU_ROW_BOX_HEIGHT, ROW_RADIUS,
		                     COLOR_TILE_SELECTED, DEPTH_PANEL);
		C2D_DrawRectSolid(PANEL_ROW_X, y + MENU_ROW_MARK_Y, DEPTH_TILE, ROW_MARK_WIDTH, MENU_ROW_MARK_HEIGHT,
		                  COLOR_ACCENT);
	}
	u32 labelColor = menu_label_color(shot_mode_row_disabled_reason(row) != NULL, selected);
	ui_text_draw(g_fontBody, PANEL_ROW_TEXT_X, y + MENU_ROW_TEXT_Y, labelColor, TEXT_ALIGN_LEFT, "%s",
	             row->label);
	float middleY = y + MENU_ROW_MIDDLE_Y;
	if (row->kind == ROW_ACTION) {
		u32 pointerColor = selected ? COLOR_ACCENT : COLOR_FAINT;
		ui_shapes_pointer(MENU_ACTION_POINTER_X, middleY, MENU_ACTION_POINTER_SIZE, POINTER_DIRECTION_RIGHT,
		                  pointerColor, DEPTH_CONTROL);
		return;
	}
	char value[MENU_ROW_VALUE_SIZE];
	settings_row_value(row, value, sizeof value, false);
	float valueRightX = selected ? MENU_SELECTED_VALUE_X : PANEL_VALUE_X;
	u32 valueColor = selected ? COLOR_ACCENT : COLOR_MUTED;
	float valueWidth = ui_text_draw(g_fontBody, valueRightX, y + MENU_ROW_TEXT_Y, valueColor,
	                                TEXT_ALIGN_RIGHT, "%s", value);
	if (!selected) {
		return;
	}
	// Arrows on both sides of the selected value.
	float previousPointerX = MENU_SELECTED_VALUE_X - valueWidth - MENU_PREVIOUS_POINTER_GAP;
	ui_shapes_pointer(MENU_NEXT_POINTER_X, middleY, MENU_VALUE_POINTER_SIZE, POINTER_DIRECTION_RIGHT,
	                  COLOR_ACCENT, DEPTH_CONTROL);
	ui_shapes_pointer(previousPointerX, middleY, MENU_VALUE_POINTER_SIZE, POINTER_DIRECTION_LEFT,
	                  COLOR_ACCENT, DEPTH_CONTROL);
}

static void draw_setup_menu(void)
{
	draw_header("SETUP");
	draw_menu_tabs();
	const SettingPage *page = &SETTINGS_PAGES[g_menuPage];
	int topRow = g_menuTopRow[g_menuPage];
	for (int i = topRow; i < page->count && i < topRow + MENU_VISIBLE_ROWS; i++) {
		float y = MENU_ROW_Y + (i - topRow) * MENU_ROW_HEIGHT;
		bool selected = i == g_menuSelection[g_menuPage];
		draw_menu_row(&page->rows[i], y, selected);
	}
	ui_panel_draw_scroll_marks(topRow, MENU_VISIBLE_ROWS, page->count, SCROLL_UP_MARK_Y,
	                           SCROLL_DOWN_MARK_BASE_Y);
	draw_divider(PANEL_FOOTER_LINE_Y);
	ui_text_draw(g_fontSmall, PANEL_TEXT_X, PANEL_FOOTER_TEXT_Y, COLOR_MUTED, TEXT_ALIGN_LEFT,
	             "A select   B back   L / R section");
	if (g_menuPage == SETTINGS_PAGE_SYSTEM) {
		ui_text_draw(g_fontSmall, PANEL_TEXT_X, MENU_VERSION_Y, COLOR_FAINT, TEXT_ALIGN_LEFT,
		             "Version " APP_VERSION);
	}
	draw_shutter_button();
}

void menus_draw(void)
{
	C2D_DrawRectSolid(0, 0, 0, BOTTOM_SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
	if (gallery_is_open()) {
		gallery_panel_draw();
	} else if (look_editor_active()) {
		look_editor_draw();
	} else if (value_picker_active()) {
		value_picker_draw();
	} else if (g_infoOpen) {
		draw_camera_info();
	} else if (g_menuOpen) {
		draw_setup_menu();
	} else {
		draw_quick_menu();
	}
}

static void setup_menu_touch(touchPosition touch, const SettingPage *page, int *selection, bool *quit)
{
	if (touch.py < PANEL_SUBHEADER_LINE_Y) {
		g_menuPage = math_clamp_int(touch.px / MENU_TAB_WIDTH, 0, SETTINGS_PAGE_COUNT - 1);
		return;
	}
	if (menus_touch_on_shutter(touch)) {
		g_menuOpen = false;
		self_timer_press();
		return;
	}
	if (touch.py < MENU_ROW_Y || touch.py >= MENU_ROWS_BOTTOM) {
		return;
	}
	int rowIndex = g_menuTopRow[g_menuPage] + (touch.py - MENU_ROW_Y) / MENU_ROW_HEIGHT;
	if (rowIndex >= page->count) {
		return;
	}
	// A first tap selects the row, a second one opens its values.
	bool secondTap = rowIndex == *selection;
	*selection = rowIndex;
	const SettingRow *row = &page->rows[rowIndex];
	if (row->kind == ROW_ACTION) {
		setting_actions_run(row->action, quit);
	} else if (secondTap) {
		value_picker_open(row);
	}
}

void menus_setup_input(u32 down, u32 held, touchPosition touch, bool *quit)
{
	if (down & (KEY_B | KEY_START)) {
		g_menuOpen = false;
		return;
	}
	if (down & KEY_R) {
		g_menuPage = (g_menuPage + 1) % SETTINGS_PAGE_COUNT;
	}
	if (down & KEY_L) {
		g_menuPage = (g_menuPage + SETTINGS_PAGE_COUNT - 1) % SETTINGS_PAGE_COUNT;
	}
	const SettingPage *page = &SETTINGS_PAGES[g_menuPage];
	int *selection = &g_menuSelection[g_menuPage];
	if (key_repeat_dpad(held, KEY_DDOWN, LIST_REPEAT_FRAMES)) {
		*selection = (*selection + 1) % page->count;
	}
	if (key_repeat_dpad(held, KEY_DUP, LIST_REPEAT_FRAMES)) {
		*selection = (*selection + page->count - 1) % page->count;
	}
	keep_selection_visible();
	const SettingRow *row = &page->rows[*selection];
	if (key_repeat_dpad(held, KEY_DRIGHT, LIST_REPEAT_FRAMES)) {
		setting_actions_step_row(row, 1);
	}
	if (key_repeat_dpad(held, KEY_DLEFT, LIST_REPEAT_FRAMES)) {
		setting_actions_step_row(row, -1);
	}
	if (down & KEY_A) {
		if (row->kind == ROW_ACTION) {
			setting_actions_run(row->action, quit);
		} else {
			value_picker_open(row);
		}
	}
	if (down & KEY_TOUCH) {
		setup_menu_touch(touch, page, selection, quit);
		keep_selection_visible();
	}
}
