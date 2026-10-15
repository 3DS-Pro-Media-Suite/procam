#include "input.h"
#include <3ds.h>
#include <math.h>
#include <stdlib.h>
#include "cam_settings.h"
#include "cam_stream.h"
#include "camera_control.h"
#include "gallery.h"
#include "gallery_panel.h"
#include "key_repeat.h"
#include "look_editor.h"
#include "menus.h"
#include "metering.h"
#include "review.h"
#include "self_timer.h"
#include "setting_actions.h"
#include "settings.h"
#include "toast.h"
#include "touch.h"
#include "ui_layout.h"
#include "value_picker.h"

// Key repeat periods in frames.
#define SHOOTING_REPEAT_FRAMES 5
#define SECOND_DIAL_REPEAT_FRAMES 6
#define REAR_DIAL_FAST_REPEAT_FRAMES 3
#define REAR_DIAL_SLOW_REPEAT_FRAMES 7
#define TOUCH_ARROW_REPEAT_FRAMES 4

// Holding SELECT longer than this shows the button hints. A shorter tap shows the last photo.
#define SELECT_HOLD_MS 350
#define LAST_PHOTO_REVIEW_MS 3000

// A Circle Pad axis counts past the dead zone. A push past the fast limit turns the rear dial faster.
#define CIRCLE_PAD_DEAD_ZONE 60
#define CIRCLE_PAD_FAST_PUSH 130

// A tap this close to the metering point, as a fraction of the screen, clears it.
#define TOUCH_METER_MARKER_SIZE 0.1f

bool g_showControls;

// Hold counters for key repeat. The D-pad's are shared with the other screens (key_repeat_dpad).
static int g_repeatRearDial;
static int g_repeatBrighter;
static int g_repeatDarker;
static int g_repeatTouchArrow;
static int g_repeatSecondDial;

static u64 g_selectPressedAt;
static bool g_selectUsedInCombo;

static bool gallery_button_tapped(u32 down, touchPosition touch)
{
	// The viewfinder covers the button when it is on the touch screen.
	bool viewfinderOnTouchScreen = g_screensSwapped && !g_menuOpen;
	return (down & KEY_TOUCH) && !self_timer_counting() && !viewfinderOnTouchScreen &&
	       touch_in_rect(touch, GALLERY_BUTTON_X, GALLERY_BUTTON_Y, GALLERY_BUTTON_WIDTH,
	                     GALLERY_BUTTON_HEIGHT);
}

// SELECT: a hold shows the button hints, a tap shows the last photo and SELECT + L swaps the screens.
// Returns the pressed keys without an L that SELECT + L used.
static u32 select_button_input(u32 down, u32 held)
{
	u32 released = hidKeysUp();
	u64 nowMs = osGetTime();
	if (down & KEY_SELECT) {
		g_selectPressedAt = nowMs;
		g_selectUsedInCombo = false;
	}
	g_showControls = (held & KEY_SELECT) && nowMs - g_selectPressedAt > SELECT_HOLD_MS;
	if ((held & KEY_SELECT) && (down & KEY_L)) {
		setting_actions_swap_screens();
		g_selectUsedInCombo = true;
		down &= ~KEY_L;
	}
	if ((released & KEY_SELECT) && !g_selectUsedInCombo && nowMs - g_selectPressedAt <= SELECT_HOLD_MS) {
		review_request(LAST_PHOTO_REVIEW_MS);
	}
	return down;
}

static void toggle_exposure_lock(void)
{
	g_settings.cam.autoExposure = !g_settings.cam.autoExposure;
	setting_actions_on_change(settings_ae_lock_row());
	toast_show(g_settings.cam.autoExposure ? "Exposure: auto" : "Exposure locked");
}

// Sticks act on one axis at a time, so a slightly diagonal push does not change two settings at once.
// A Circle Pad axis counts when it is past the dead zone and at least 1.5 times the other axis.
static int circle_pad_axis_step(int value, int otherValue)
{
	int size = abs(value);
	int otherSize = abs(otherValue);
	if (size <= CIRCLE_PAD_DEAD_ZONE || size * 2 <= otherSize * 3) {
		return 0;
	}
	return value > 0 ? 1 : -1;
}

// The C-stick only reports direction bits. A diagonal sets two bits and counts as none.
static int cstick_axis_step(u32 held, u32 positiveKey, u32 negativeKey, u32 otherAxisKeys)
{
	bool onAxis = held & (positiveKey | negativeKey);
	bool onOtherAxis = held & otherAxisKeys;
	if (!onAxis || onOtherAxis) {
		return 0;
	}
	return (held & positiveKey) ? 1 : -1;
}

// X / Y and the C-stick sideways make the picture brighter or darker: EV, or the exposure time when the
// main dial sets it. Then the Circle Pad or the C-stick up / down turns the second dial: ISO in manual,
// EV in shutter priority.
static void turn_exposure_dials(u32 held, circlePosition circle, bool exposureTimeDial)
{
	const SettingRow *mainDial =
	        exposureTimeDial ? &g_quickRows[QUICK_TILE_SHUTTER] : &g_quickRows[QUICK_TILE_EV];
	int padVertical = circle_pad_axis_step(circle.dy, circle.dx);
	int cstickX = cstick_axis_step(held, KEY_CSTICK_RIGHT, KEY_CSTICK_LEFT, KEY_CSTICK_UP | KEY_CSTICK_DOWN);
	int cstickY = cstick_axis_step(held, KEY_CSTICK_UP, KEY_CSTICK_DOWN, KEY_CSTICK_LEFT | KEY_CSTICK_RIGHT);
	if (key_repeat_fires((held & KEY_X) || cstickX > 0, &g_repeatBrighter, SHOOTING_REPEAT_FRAMES)) {
		setting_actions_step_row(mainDial, 1);
	}
	if (key_repeat_fires((held & KEY_Y) || cstickX < 0, &g_repeatDarker, SHOOTING_REPEAT_FRAMES)) {
		setting_actions_step_row(mainDial, -1);
	}
	int secondDialStep = padVertical != 0 ? padVertical : cstickY;
	if (!exposureTimeDial) {
		return;
	}
	if (!key_repeat_fires(secondDialStep != 0, &g_repeatSecondDial, SECOND_DIAL_REPEAT_FRAMES)) {
		return;
	}
	bool manual = g_settings.cam.exposureMode == CAM_EXPOSURE_MANUAL;
	const SettingRow *secondDial = manual ? &g_quickRows[QUICK_TILE_ISO] : &g_quickRows[QUICK_TILE_EV];
	setting_actions_step_row(secondDial, secondDialStep);
}

// The D-pad moves the cursor over the quick menu tiles and wraps at the edges.
static void move_quick_cursor(u32 held)
{
	int column = g_quickSelection % QUICK_GRID_COLUMNS;
	int row = g_quickSelection / QUICK_GRID_COLUMNS;
	if (key_repeat_dpad(held, KEY_DRIGHT, SHOOTING_REPEAT_FRAMES)) {
		column = (column + 1) % QUICK_GRID_COLUMNS;
	}
	if (key_repeat_dpad(held, KEY_DLEFT, SHOOTING_REPEAT_FRAMES)) {
		column = (column + QUICK_GRID_COLUMNS - 1) % QUICK_GRID_COLUMNS;
	}
	if (key_repeat_dpad(held, KEY_DDOWN, SHOOTING_REPEAT_FRAMES)) {
		row = (row + 1) % QUICK_GRID_ROWS;
	}
	if (key_repeat_dpad(held, KEY_DUP, SHOOTING_REPEAT_FRAMES)) {
		row = (row + QUICK_GRID_ROWS - 1) % QUICK_GRID_ROWS;
	}
	g_quickSelection = row * QUICK_GRID_COLUMNS + column;
}

// The Circle Pad sideways is the rear dial: it turns the selected setting, faster with a further push.
static void turn_rear_dial(circlePosition circle)
{
	int dialStep = circle_pad_axis_step(circle.dx, circle.dy);
	bool fastPush = abs(circle.dx) > CIRCLE_PAD_FAST_PUSH;
	int repeatFrames = fastPush ? REAR_DIAL_FAST_REPEAT_FRAMES : REAR_DIAL_SLOW_REPEAT_FRAMES;
	if (key_repeat_fires(dialStep != 0, &g_repeatRearDial, repeatFrames)) {
		setting_actions_step_row(&g_quickRows[g_quickSelection], dialStep);
	}
}

// With the viewfinder on the touch screen a tap meters there. A tap on the marker goes back to the
// metering window.
static void meter_at_touch(touchPosition touch)
{
	// The picture fills the screen, mirrored as it is displayed.
	int flip = cam_stream_raw_active() ? g_settings.cam.flip : 0;
	float meterX = touch.px / (float)BOTTOM_SCREEN_WIDTH;
	float meterY = touch.py / (float)SCREEN_HEIGHT;
	if (flip & FLIP_HORIZONTAL) {
		meterX = 1 - meterX;
	}
	if (flip & FLIP_VERTICAL) {
		meterY = 1 - meterY;
	}
	float markerX;
	float markerY;
	bool haveMarker = metering_touch_point(&markerX, &markerY);
	bool onMarker = haveMarker && fabsf(meterX - markerX) < TOUCH_METER_MARKER_SIZE &&
	                fabsf(meterY - markerY) < TOUCH_METER_MARKER_SIZE;
	if (onMarker) {
		metering_clear_touch_point();
		toast_show("Metering: %s", CAM_METERING_WINDOW_NAMES[(int)g_settings.cam.aeWindow]);
	} else {
		metering_set_touch_point(meterX, meterY);
	}
}

static int touched_arrow(touchPosition touch)
{
	if (touch_in_rect(touch, ARROW_LEFT_X, ARROW_Y, ARROW_WIDTH, ARROW_HEIGHT)) {
		return -1;
	}
	if (touch_in_rect(touch, ARROW_RIGHT_X, ARROW_Y, ARROW_WIDTH, ARROW_HEIGHT)) {
		return 1;
	}
	return 0;
}

// The quick menu on the touch screen: the value arrows, the shutter button and the tiles.
static void quick_menu_touch(u32 down, u32 held, touchPosition touch)
{
	if (held & KEY_TOUCH) {
		int arrowStep = touched_arrow(touch);
		if (key_repeat_fires(arrowStep != 0, &g_repeatTouchArrow, TOUCH_ARROW_REPEAT_FRAMES)) {
			setting_actions_step_row(&g_quickRows[g_quickSelection], arrowStep);
		}
	} else {
		g_repeatTouchArrow = 0;
	}
	if (!(down & KEY_TOUCH)) {
		return;
	}
	if (menus_touch_on_shutter(touch)) {
		self_timer_press();
	}
	for (int i = 0; i < QUICK_TILE_COUNT; i++) {
		float tileX;
		float tileY;
		menus_quick_tile_rect(i, &tileX, &tileY);
		if (touch_in_rect(touch, tileX, tileY, QUICK_TILE_WIDTH, QUICK_TILE_HEIGHT)) {
			// A first tap selects the tile, a second one shows all its values.
			if (i == g_quickSelection) {
				value_picker_open(&g_quickRows[i]);
			}
			g_quickSelection = i;
		}
	}
}

// One layout for Old and New 3DS. ZL, ZR and the C-stick of a New 3DS only add shortcuts.
static void shooting_input(u32 down, u32 held, touchPosition touch)
{
	down = select_button_input(down, held);
	if (down & KEY_ZL) {
		setting_actions_swap_screens();
	}
	if (down & (KEY_A | KEY_R | KEY_ZR)) {
		self_timer_press();
	}
	if (down & KEY_L) {
		toggle_exposure_lock();
	}
	if (down & KEY_B) {
		g_settings.hud = (g_settings.hud + HUD_LEVEL_COUNT - 1) % HUD_LEVEL_COUNT;
	}
	if (down & KEY_START) {
		g_menuOpen = true;
		return;
	}
	bool exposureTimeDial = cam_stream_raw_active() && g_settings.cam.exposureMode != CAM_EXPOSURE_AUTO;
	circlePosition circle = {0};
	hidCircleRead(&circle);
	turn_exposure_dials(held, circle, exposureTimeDial);
	move_quick_cursor(held);
	turn_rear_dial(circle);
	if (g_screensSwapped) {
		if (down & KEY_TOUCH) {
			meter_at_touch(touch);
		}
		return;
	}
	quick_menu_touch(down, held, touch);
}

// Shooting: the D-pad moves the quick menu cursor, the Circle Pad sideways turns the selected setting,
// X / Y or the C-stick set the exposure, A / R / ZR fire, L locks the exposure, B cycles the information
// level, START opens the setup menu and SELECT shows the last photo. START + SELECT exits.
void input_handle(bool *quit)
{
	u32 down = hidKeysDown();
	u32 held = hidKeysHeld();
	touchPosition touch = {0};
	if (held & KEY_TOUCH) {
		hidTouchRead(&touch);
	}
	if ((held & KEY_START) && (held & KEY_SELECT)) {
		*quit = true;
		return;
	}
	if (!camera_control_ok()) {
		return;
	}
	// Any key ends the photo review.
	if (down && review_showing(osGetTime())) {
		review_hide();
		return;
	}
	if (gallery_is_open()) {
		gallery_panel_input(down, touch);
		return;
	}
	if (g_infoOpen) {
		if (down & (KEY_A | KEY_B | KEY_START | KEY_TOUCH)) {
			g_infoOpen = false;
		}
		return;
	}
	if (look_editor_active()) {
		look_editor_input(down, held, touch);
		return;
	}
	if (value_picker_active()) {
		value_picker_input(down, held, touch);
		return;
	}
	if (gallery_button_tapped(down, touch)) {
		g_menuOpen = false;
		gallery_open();
		return;
	}
	if (g_menuOpen) {
		menus_setup_input(down, held, touch, quit);
		return;
	}
	shooting_input(down, held, touch);
}
