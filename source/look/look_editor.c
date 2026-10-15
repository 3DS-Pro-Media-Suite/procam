#include "look_editor.h"
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "camera_control.h"
#include "key_repeat.h"
#include "look.h"
#include "look_editor_internal.h"
#include "look_test_render.h"
#include "math_util.h"
#include "raw_view.h"
#include "settings.h"
#include "shot_mode.h"
#include "toast.h"
#include "touch.h"
#include "ui_layout.h"
#include "value_picker.h"

// The look editor on the bottom screen. Every change goes live at once. Nothing reaches the card until Save,
// and Discard brings back the card's version. SELECT renders the last raw frame through every step, including
// the steps the live view cannot show.

#define DEFAULT_PARAM_STEP 0.01f // for parameters whose table gives no step
#define CURVE_START_VALUES 8 // a new curve has four x:y points on the diagonal

#define KEYBOARD_BUTTON_COUNT 2 // Cancel and OK
#define KEYBOARD_TEXT_SIZE 64
#define SHORT_NAME_MAX_LETTERS 8
#define SHORT_NAME_HINT_SIZE 64
#define COPY_NAME_SUFFIX " 2"
#define COPY_NAME_KEPT_LETTERS 26 // of the old name, in the copy's suggested name

// Key repeat: frames between repeats while a button is held.
#define LIST_REPEAT_FRAMES 4
#define VALUE_REPEAT_FRAMES 2
#define GRID_REPEAT_FRAMES 5

// The Circle Pad nudges a value by a few steps, or by many when pushed far.
#define CIRCLE_PAD_DEAD_ZONE 40
#define CIRCLE_PAD_FAR 120
#define CIRCLE_PAD_NEAR_STEPS 3
#define CIRCLE_PAD_FAR_STEPS 10

#define SLIDER_TOUCH_MARGIN 8 // a touch this far beyond a slider's end still grabs it

#define ADD_TILE_COLUMNS 3
#define ADD_TILE_ROWS ((LOOK_OP_KINDS + ADD_TILE_COLUMNS - 1) / ADD_TILE_COLUMNS)
#define ADD_TILE_WIDTH 100
#define ADD_TILE_HEIGHT 30

#define SAVE_ROW_FIRST_Y 30

LookEditor g_lookEditor = {.draggedParam = -1};

static int g_repeatCirclePad; // the Circle Pad's hold counter for key repeat

// The editor's own D-pad hold counters, shared by its pages. A direction held while the editor opens or
// closes starts as a fresh press on the other side.
static int g_repeatUp;
static int g_repeatDown;
static int g_repeatLeft;
static int g_repeatRight;

// key_repeat_dpad on the editor's own counters.
static bool editor_dpad_fires(u32 held, u32 dpadKey, int repeatEvery)
{
	int *heldFrames = &g_repeatRight;
	if (dpadKey == KEY_DUP) {
		heldFrames = &g_repeatUp;
	} else if (dpadKey == KEY_DDOWN) {
		heldFrames = &g_repeatDown;
	} else if (dpadKey == KEY_DLEFT) {
		heldFrames = &g_repeatLeft;
	}
	return key_repeat_fires((held & dpadKey) != 0, heldFrames, repeatEvery);
}

bool look_editor_active(void)
{
	return g_lookEditor.page != EDITOR_PAGE_OFF;
}

void look_editor_open(void)
{
	if (!look_count()) {
		toast_show("No looks loaded");
		return;
	}
	const char *disabledReason = shot_mode_row_disabled_reason(&g_quickRows[QUICK_TILE_LOOK]);
	if (disabledReason) {
		toast_show("%s", disabledReason);
		return;
	}
	g_lookEditor.lookIndex = math_clamp_int(g_settings.lookIndex, 0, look_count() - 1);
	g_lookEditor.saved = *look_get(g_lookEditor.lookIndex);
	g_lookEditor.edited = g_lookEditor.saved;
	g_lookEditor.unsavedChanges = false;
	g_lookEditor.page = EDITOR_PAGE_STEPS;
	g_lookEditor.stepSelection = 0;
	g_lookEditor.firstVisibleStep = 0;
	g_lookEditor.paramSelection = 0;
	g_lookEditor.firstVisibleParam = 0;
	g_lookEditor.draggedParam = -1;
	value_picker_close();
}

static void apply_edited_look(void)
{
	look_set(g_lookEditor.lookIndex, &g_lookEditor.edited);
	g_lookRevision++;
	g_lookEditor.unsavedChanges = true;
}

static void open_save_page(void)
{
	g_lookEditor.page = EDITOR_PAGE_SAVE;
	g_lookEditor.saveSelection = 0;
}

static void open_params_page(void)
{
	if (!g_lookEditor.edited.opCount) {
		return;
	}
	g_lookEditor.page = EDITOR_PAGE_PARAMS;
	g_lookEditor.paramSelection = 0;
	g_lookEditor.firstVisibleParam = 0;
	g_lookEditor.draggedParam = -1;
}

float look_editor_param_step(const LookOpInfo *info, int param)
{
	return info->step[param] > 0 ? info->step[param] : DEFAULT_PARAM_STEP;
}

// Sets a parameter of the selected step, rounded to its step and kept in its range.
static void set_param(int param, float value)
{
	LookOp *step = &g_lookEditor.edited.ops[g_lookEditor.stepSelection];
	const LookOpInfo *info = &LOOK_OP_INFO[step->kind];
	float increment = look_editor_param_step(info, param);
	value = roundf(value / increment) * increment;
	value = math_clamp_float(value, info->min[param], info->max[param]);
	if (fabsf(value) < increment / 2) {
		value = 0; // no "-0.00"
	}
	if (value == step->values[param]) {
		return;
	}
	step->values[param] = value;
	apply_edited_look();
}

static void nudge_param(int param, float increments)
{
	const LookOp *step = &g_lookEditor.edited.ops[g_lookEditor.stepSelection];
	float increment = look_editor_param_step(&LOOK_OP_INFO[step->kind], param);
	set_param(param, step->values[param] + increments * increment);
}

static void reset_selected_param(const LookOp *step)
{
	int param = g_lookEditor.paramSelection;
	set_param(param, LOOK_OP_INFO[step->kind].defaults[param]);
}

static void select_next_param(int paramCount)
{
	g_lookEditor.paramSelection = (g_lookEditor.paramSelection + 1) % paramCount;
}

// Inserts a new step after the selected one and opens its parameters.
static void add_step(LookOpKind kind)
{
	Look *look = &g_lookEditor.edited;
	if (look->opCount >= LOOK_OPS_MAX) {
		toast_show("A look holds %d steps at most", LOOK_OPS_MAX);
		return;
	}
	int position = look->opCount ? g_lookEditor.stepSelection + 1 : 0;
	if (kind == LOOK_OP_HOT_PIXEL) {
		position = 0; // it works on the raw data, before every other step
	}
	memmove(&look->ops[position + 1], &look->ops[position], (look->opCount - position) * sizeof(LookOp));
	LookOp *step = &look->ops[position];
	memset(step, 0, sizeof *step);
	step->kind = kind;
	const LookOpInfo *info = &LOOK_OP_INFO[kind];
	step->count = kind == LOOK_OP_CURVE ? CURVE_START_VALUES : info->paramCount;
	for (int j = 0; j < step->count; j++) {
		step->values[j] = info->defaults[j];
	}
	look->opCount++;
	g_lookEditor.stepSelection = position;
	apply_edited_look();
	open_params_page();
}

static void remove_step(void)
{
	Look *look = &g_lookEditor.edited;
	if (!look->opCount) {
		return;
	}
	const char *name = LOOK_OP_INFO[look->ops[g_lookEditor.stepSelection].kind].name;
	toast_show("Removed %s (Discard in Save brings it back)", name);
	int selection = g_lookEditor.stepSelection;
	memmove(&look->ops[selection], &look->ops[selection + 1],
	        (look->opCount - selection - 1) * sizeof(LookOp));
	look->opCount--;
	if (g_lookEditor.stepSelection >= look->opCount) {
		g_lookEditor.stepSelection = look->opCount ? look->opCount - 1 : 0;
	}
	apply_edited_look();
}

// Swaps the selected step with its neighbour above (-1) or below (1).
static void move_step(int direction)
{
	Look *look = &g_lookEditor.edited;
	int selection = g_lookEditor.stepSelection;
	int neighbour = selection + direction;
	if (!look->opCount || neighbour < 0 || neighbour >= look->opCount) {
		return;
	}
	LookOp moved = look->ops[selection];
	look->ops[selection] = look->ops[neighbour];
	look->ops[neighbour] = moved;
	g_lookEditor.stepSelection = neighbour;
	apply_edited_look();
}

// The system keyboard. '#' would start a comment in the recipe file and a line break would end the line.
static bool ask_text(const char *hint, const char *initial, int maxLength, char *out, size_t outSize)
{
	SwkbdState keyboard;
	swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, KEYBOARD_BUTTON_COUNT, maxLength);
	swkbdSetHintText(&keyboard, hint);
	swkbdSetInitialText(&keyboard, initial);
	swkbdSetValidation(&keyboard, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
	char text[KEYBOARD_TEXT_SIZE] = {0};
	camera_control_set_in_applet(true);
	SwkbdButton button = swkbdInputText(&keyboard, text, sizeof text);
	camera_control_set_in_applet(false);
	if (button != SWKBD_BUTTON_CONFIRM) {
		return false;
	}
	for (char *p = text; *p; p++) {
		if (*p == '#' || *p == '\n' || *p == '\r') {
			*p = ' ';
		}
	}
	snprintf(out, outSize, "%s", text);
	return true;
}

static char ascii_upper(char c)
{
	if (c >= 'a' && c <= 'z') {
		return c - ('a' - 'A');
	}
	return c;
}

// Asks for the name and the LOOK tile's short name. A new name suggests a new short name.
static bool ask_names(Look *look, const char *initialName)
{
	char name[sizeof look->name];
	char shortName[sizeof look->shortName];
	char suggestion[sizeof look->shortName];
	if (!ask_text("Look name", initialName, sizeof name - 1, name, sizeof name)) {
		return false;
	}
	if (!strcmp(name, look->name)) {
		snprintf(suggestion, sizeof suggestion, "%s", look->shortName);
	} else {
		int length = 0;
		for (const char *p = name; *p && length < SHORT_NAME_MAX_LETTERS; p++) {
			suggestion[length] = ascii_upper(*p);
			length++;
		}
		suggestion[length] = 0;
	}
	char hint[SHORT_NAME_HINT_SIZE];
	snprintf(hint, sizeof hint, "Short name for the LOOK tile (%d letters)", SHORT_NAME_MAX_LETTERS);
	if (!ask_text(hint, suggestion, SHORT_NAME_MAX_LETTERS, shortName, sizeof shortName)) {
		return false;
	}
	snprintf(look->name, sizeof look->name, "%s", name);
	snprintf(look->shortName, sizeof look->shortName, "%s", shortName);
	return true;
}

static void save_over_file(void)
{
	if (!look_save(g_lookEditor.lookIndex)) {
		toast_show("Could not write %s", g_lookEditor.edited.file);
		return;
	}
	g_lookEditor.saved = g_lookEditor.edited;
	g_lookEditor.unsavedChanges = false;
	g_lookEditor.page = EDITOR_PAGE_OFF;
	toast_show("Saved %s", g_lookEditor.edited.file);
}

// Saves the edited recipe as a new file. The original look gets its saved recipe back.
static void save_as_new_look(void)
{
	Look copy = g_lookEditor.edited;
	char initialName[sizeof copy.name];
	// The suggested name: the old one, cut short, and " 2".
	int nameRoom = COPY_NAME_KEPT_LETTERS;
	snprintf(initialName, sizeof initialName, "%.*s" COPY_NAME_SUFFIX, nameRoom, g_lookEditor.edited.name);
	if (!ask_names(&copy, initialName)) {
		return;
	}
	look_set(g_lookEditor.lookIndex, &g_lookEditor.saved);
	int newIndex = look_add(&copy);
	if (newIndex < 0) {
		look_set(g_lookEditor.lookIndex, &g_lookEditor.edited);
		const char *reason =
		        look_count() >= LOOKS_MAX ? "No room: %d looks at most" : "Could not write the new look";
		toast_show(reason, LOOKS_MAX);
		return;
	}
	settings_refresh_look_tile();
	g_settings.lookIndex = newIndex;
	settings_save();
	g_lookEditor.lookIndex = newIndex;
	g_lookEditor.saved = *look_get(newIndex);
	g_lookEditor.edited = g_lookEditor.saved;
	g_lookEditor.unsavedChanges = false;
	g_lookRevision++;
	g_lookEditor.page = EDITOR_PAGE_STEPS;
	toast_show("Saved as %s (%s)", g_lookEditor.edited.name, g_lookEditor.edited.file);
}

// The new names are written with the next Save.
static void rename_look(void)
{
	if (ask_names(&g_lookEditor.edited, g_lookEditor.edited.name)) {
		apply_edited_look();
	}
	g_lookEditor.page = EDITOR_PAGE_STEPS;
}

static void discard_changes(void)
{
	look_set(g_lookEditor.lookIndex, &g_lookEditor.saved);
	g_lookRevision++;
	g_lookEditor.edited = g_lookEditor.saved;
	g_lookEditor.unsavedChanges = false;
	g_lookEditor.page = EDITOR_PAGE_OFF;
	toast_show("Changes discarded");
}

static void choose_save_option(int option)
{
	switch (option) {
	case SAVE_OPTION_SAVE:
		save_over_file();
		break;
	case SAVE_OPTION_SAVE_AS_NEW:
		save_as_new_look();
		break;
	case SAVE_OPTION_RENAME:
		rename_look();
		break;
	case SAVE_OPTION_DISCARD:
		discard_changes();
		break;
	default:
		g_lookEditor.page = EDITOR_PAGE_STEPS;
		break;
	}
}

// Returns the button under the touch in a row of buttonCount buttons, or -1.
static int button_at(touchPosition touch, int buttonCount)
{
	if (touch.py < EDITOR_BUTTON_Y || touch.py >= EDITOR_BUTTON_Y + EDITOR_BUTTON_HEIGHT) {
		return -1;
	}
	float buttonWidth = ((float)PANEL_ROW_WIDTH - (buttonCount - 1) * TILE_GAP) / buttonCount;
	int button = (int)((touch.px - PANEL_ROW_X) / (buttonWidth + TILE_GAP));
	return button >= 0 && button < buttonCount ? button : -1;
}

void look_editor_add_tile_rect(int kind, float *x, float *y, float *width, float *height)
{
	int column = kind % ADD_TILE_COLUMNS;
	int row = kind / ADD_TILE_COLUMNS;
	*width = ADD_TILE_WIDTH;
	*height = ADD_TILE_HEIGHT;
	*x = PANEL_ROW_X + column * (ADD_TILE_WIDTH + TILE_GAP);
	*y = PANEL_CONTENT_Y + row * (ADD_TILE_HEIGHT + TILE_GAP);
}

float look_editor_save_row_y(int option)
{
	return SAVE_ROW_FIRST_Y + option * (EDITOR_SAVE_ROW_HEIGHT + TILE_GAP);
}

// The first tap on a step selects it, a second one opens it.
static void tap_step_row(touchPosition touch, int stepCount)
{
	int step = g_lookEditor.firstVisibleStep + (touch.py - EDITOR_STEP_ROW_Y) / EDITOR_STEP_ROW_HEIGHT;
	if (step >= stepCount) {
		return;
	}
	bool secondTap = step == g_lookEditor.stepSelection;
	g_lookEditor.stepSelection = step;
	if (secondTap) {
		open_params_page();
	}
}

static void steps_page_touch(touchPosition touch, int stepCount)
{
	int button = button_at(touch, STEPS_BUTTON_COUNT);
	if (button == STEPS_BUTTON_ADD) {
		g_lookEditor.page = EDITOR_PAGE_ADD;
	} else if (button == STEPS_BUTTON_REMOVE) {
		remove_step();
	} else if (button == STEPS_BUTTON_UP) {
		move_step(-1);
	} else if (button == STEPS_BUTTON_DOWN) {
		move_step(1);
	} else if (button == STEPS_BUTTON_SAVE) {
		open_save_page();
	} else if (touch.py >= EDITOR_STEP_ROW_Y && touch.py < EDITOR_STEP_LIST_BOTTOM) {
		tap_step_row(touch, stepCount);
	}
}

static void steps_page_input(u32 down, u32 held, touchPosition touch)
{
	int stepCount = g_lookEditor.edited.opCount;
	if (down & KEY_B) {
		if (g_lookEditor.unsavedChanges) {
			open_save_page();
		} else {
			g_lookEditor.page = EDITOR_PAGE_OFF;
		}
		return;
	}
	if (down & KEY_START) {
		open_save_page();
		return;
	}
	if (stepCount && editor_dpad_fires(held, KEY_DDOWN, LIST_REPEAT_FRAMES)) {
		g_lookEditor.stepSelection = (g_lookEditor.stepSelection + 1) % stepCount;
	}
	if (stepCount && editor_dpad_fires(held, KEY_DUP, LIST_REPEAT_FRAMES)) {
		g_lookEditor.stepSelection = (g_lookEditor.stepSelection + stepCount - 1) % stepCount;
	}
	if (down & KEY_A) {
		open_params_page();
		return;
	}
	if (down & KEY_X) {
		g_lookEditor.page = EDITOR_PAGE_ADD;
		return;
	}
	if (down & KEY_Y) {
		remove_step();
	}
	if (down & KEY_L) {
		move_step(-1);
	}
	if (down & KEY_R) {
		move_step(1);
	}
	if (down & KEY_TOUCH) {
		steps_page_touch(touch, stepCount);
	}
}

// A tap on a parameter selects it. A tap on its slider also starts dragging the value.
static void tap_param_row(touchPosition touch, int paramCount)
{
	int param = g_lookEditor.firstVisibleParam + (touch.py - EDITOR_PARAM_ROW_Y) / EDITOR_PARAM_ROW_HEIGHT;
	if (param >= paramCount) {
		return;
	}
	g_lookEditor.paramSelection = param;
	if (touch.px >= EDITOR_SLIDER_LEFT - SLIDER_TOUCH_MARGIN &&
	    touch.px < EDITOR_SLIDER_RIGHT + SLIDER_TOUCH_MARGIN) {
		g_lookEditor.draggedParam = param;
	}
}

static void drag_slider(touchPosition touch, const LookOp *step)
{
	int param = g_lookEditor.draggedParam;
	const LookOpInfo *info = &LOOK_OP_INFO[step->kind];
	float position = (touch.px - EDITOR_SLIDER_LEFT) / (float)EDITOR_SLIDER_WIDTH;
	position = math_clamp_float(position, 0, 1);
	set_param(param, info->min[param] + position * (info->max[param] - info->min[param]));
}

static void circle_pad_input(void)
{
	circlePosition circlePad;
	hidCircleRead(&circlePad);
	bool pushed = abs(circlePad.dx) > CIRCLE_PAD_DEAD_ZONE;
	if (!key_repeat_fires(pushed, &g_repeatCirclePad, VALUE_REPEAT_FRAMES)) {
		return;
	}
	int direction = circlePad.dx > 0 ? 1 : -1;
	int increments = abs(circlePad.dx) > CIRCLE_PAD_FAR ? CIRCLE_PAD_FAR_STEPS : CIRCLE_PAD_NEAR_STEPS;
	nudge_param(g_lookEditor.paramSelection, direction * increments);
}

static void params_page_input(u32 down, u32 held, touchPosition touch)
{
	const LookOp *step = &g_lookEditor.edited.ops[g_lookEditor.stepSelection];
	int paramCount = step->count;
	if (down & KEY_B) {
		g_lookEditor.page = EDITOR_PAGE_STEPS;
		return;
	}
	if (down & KEY_START) {
		open_save_page();
		return;
	}
	if (!paramCount) {
		return;
	}
	if (editor_dpad_fires(held, KEY_DDOWN, LIST_REPEAT_FRAMES)) {
		select_next_param(paramCount);
	}
	if (editor_dpad_fires(held, KEY_DUP, LIST_REPEAT_FRAMES)) {
		g_lookEditor.paramSelection = (g_lookEditor.paramSelection + paramCount - 1) % paramCount;
	}
	if (down & KEY_A) {
		select_next_param(paramCount);
	}
	if (editor_dpad_fires(held, KEY_DRIGHT, VALUE_REPEAT_FRAMES)) {
		nudge_param(g_lookEditor.paramSelection, 1);
	}
	if (editor_dpad_fires(held, KEY_DLEFT, VALUE_REPEAT_FRAMES)) {
		nudge_param(g_lookEditor.paramSelection, -1);
	}
	circle_pad_input();
	if (down & KEY_Y) {
		reset_selected_param(step);
	}
	if (down & KEY_TOUCH) {
		int button = button_at(touch, PARAMS_BUTTON_COUNT);
		if (button == PARAMS_BUTTON_DEFAULT) {
			reset_selected_param(step);
		} else if (button == PARAMS_BUTTON_NEXT) {
			select_next_param(paramCount);
		} else if (button == PARAMS_BUTTON_BACK) {
			g_lookEditor.page = EDITOR_PAGE_STEPS;
			return;
		} else if (touch.py >= EDITOR_PARAM_ROW_Y && touch.py < EDITOR_PARAM_LIST_BOTTOM) {
			tap_param_row(touch, paramCount);
		}
	}
	bool touching = held & KEY_TOUCH;
	if (touching && g_lookEditor.draggedParam >= 0) {
		drag_slider(touch, step);
	} else if (!touching) {
		g_lookEditor.draggedParam = -1;
	}
}

// A tap on a tile selects it, a second tap adds that step.
static void add_page_touch(touchPosition touch)
{
	int button = button_at(touch, ADD_BUTTON_COUNT);
	if (button == ADD_BUTTON_ADD) {
		add_step((LookOpKind)g_lookEditor.addSelection);
		return;
	}
	if (button == ADD_BUTTON_BACK) {
		g_lookEditor.page = EDITOR_PAGE_STEPS;
		return;
	}
	for (int kind = 0; kind < LOOK_OP_KINDS; kind++) {
		float x;
		float y;
		float width;
		float height;
		look_editor_add_tile_rect(kind, &x, &y, &width, &height);
		if (touch_in_rect(touch, x, y, width, height)) {
			if (kind == g_lookEditor.addSelection) {
				add_step((LookOpKind)kind);
				return;
			}
			g_lookEditor.addSelection = kind;
		}
	}
}

static void add_page_input(u32 down, u32 held, touchPosition touch)
{
	int column = g_lookEditor.addSelection % ADD_TILE_COLUMNS;
	int row = g_lookEditor.addSelection / ADD_TILE_COLUMNS;
	if (down & KEY_B) {
		g_lookEditor.page = EDITOR_PAGE_STEPS;
		return;
	}
	if (editor_dpad_fires(held, KEY_DRIGHT, GRID_REPEAT_FRAMES)) {
		column = (column + 1) % ADD_TILE_COLUMNS;
	}
	if (editor_dpad_fires(held, KEY_DLEFT, GRID_REPEAT_FRAMES)) {
		column = (column + ADD_TILE_COLUMNS - 1) % ADD_TILE_COLUMNS;
	}
	if (editor_dpad_fires(held, KEY_DDOWN, GRID_REPEAT_FRAMES)) {
		row = (row + 1) % ADD_TILE_ROWS;
	}
	if (editor_dpad_fires(held, KEY_DUP, GRID_REPEAT_FRAMES)) {
		row = (row + ADD_TILE_ROWS - 1) % ADD_TILE_ROWS;
	}
	g_lookEditor.addSelection = math_clamp_int(row * ADD_TILE_COLUMNS + column, 0, LOOK_OP_KINDS - 1);
	if (down & KEY_A) {
		add_step((LookOpKind)g_lookEditor.addSelection);
		return;
	}
	if (down & KEY_TOUCH) {
		add_page_touch(touch);
	}
}

static void save_page_input(u32 down, u32 held, touchPosition touch)
{
	if (down & KEY_B) {
		g_lookEditor.page = EDITOR_PAGE_STEPS;
		return;
	}
	if (editor_dpad_fires(held, KEY_DDOWN, LIST_REPEAT_FRAMES)) {
		g_lookEditor.saveSelection = (g_lookEditor.saveSelection + 1) % SAVE_OPTION_COUNT;
	}
	if (editor_dpad_fires(held, KEY_DUP, LIST_REPEAT_FRAMES)) {
		g_lookEditor.saveSelection = (g_lookEditor.saveSelection + SAVE_OPTION_COUNT - 1) % SAVE_OPTION_COUNT;
	}
	if (down & KEY_A) {
		choose_save_option(g_lookEditor.saveSelection);
		return;
	}
	if (!(down & KEY_TOUCH)) {
		return;
	}
	for (int option = 0; option < SAVE_OPTION_COUNT; option++) {
		float y = look_editor_save_row_y(option);
		if (touch_in_rect(touch, PANEL_ROW_X, y, PANEL_ROW_WIDTH, EDITOR_SAVE_ROW_HEIGHT)) {
			g_lookEditor.saveSelection = option;
			choose_save_option(option);
			return;
		}
	}
}

// Scrolls a list so that its selected row is visible.
static void keep_selection_visible(int selection, int *firstVisible, int visibleRows)
{
	if (selection < *firstVisible) {
		*firstVisible = selection;
	}
	if (selection >= *firstVisible + visibleRows) {
		*firstVisible = selection - visibleRows + 1;
	}
}

void look_editor_input(u32 down, u32 held, touchPosition touch)
{
	if (down & KEY_SELECT) {
		look_test_render_start(g_lookEditor.lookIndex);
		return;
	}
	switch (g_lookEditor.page) {
	case EDITOR_PAGE_STEPS:
		steps_page_input(down, held, touch);
		break;
	case EDITOR_PAGE_PARAMS:
		params_page_input(down, held, touch);
		break;
	case EDITOR_PAGE_ADD:
		add_page_input(down, held, touch);
		break;
	case EDITOR_PAGE_SAVE:
		save_page_input(down, held, touch);
		break;
	default:
		break;
	}
	keep_selection_visible(g_lookEditor.stepSelection, &g_lookEditor.firstVisibleStep, EDITOR_STEP_ROWS);
	keep_selection_visible(g_lookEditor.paramSelection, &g_lookEditor.firstVisibleParam, EDITOR_PARAM_ROWS);
}
