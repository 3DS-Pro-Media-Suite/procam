#include "look_editor_draw.h"
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include "look.h"
#include "look_editor_internal.h"
#include "look_test_render.h"
#include "math_util.h"
#include "ui_layout.h"
#include "ui_panel.h"
#include "ui_shapes.h"
#include "ui_text.h"

#define SUBHEADER_Y 25 // the row under the title: the look's name or the step's help
#define HELP_LINE_HEIGHT 12
#define HELP_LINE_CHARS 60

#define BUTTON_LINE_GAP 4 // between the line above the buttons and the buttons
#define BUTTON_TEXT_Y 7
#define BUTTON_TEXT_PADDING 8 // left and right together

#define SCROLL_MARK_UP_GAP 1.4f // the scroll mark above a list sits this far above its first row

// Steps page.
#define LOOK_NAME_MAX_WIDTH 165
#define UNSAVED_DOT_GAP 7
#define UNSAVED_DOT_Y 33
#define UNSAVED_DOT_RADIUS 2.5f
#define LOOK_FILE_RIGHT_X 310
#define LOOK_FILE_Y 29
#define LOOK_FILE_MAX_WIDTH 120
#define LOOK_FILE_TEXT_SIZE 96
#define EMPTY_LIST_Y 110
#define STEP_ROW_BOX_HEIGHT (EDITOR_STEP_ROW_HEIGHT - 1) // 1 px between rows
#define STEP_MARK_INSET 5
#define STEP_NUMBER_X 22
#define STEP_NAME_X 34
#define STEP_NAME_Y 2
#define STEP_SMALL_TEXT_Y 5
#define STEP_TAG_GAP 6 // between a step's name and its "JPG" tag
#define STEP_VALUES_SIZE 96
#define STEP_VALUES_ROOM 12 // the step list adds values until fewer bytes than this are left
#define ROW_VALUE_MAX_WIDTH 150

// Parameters page.
#define STEP_HELP_LINES 2
#define LIVE_VIEW_NOTE_Y 49
#define TITLE_SIZE 64
#define PARAM_ROW_BOX_HEIGHT (EDITOR_PARAM_ROW_HEIGHT - 1) // 1 px between rows
#define PARAM_NAME_X 14
#define PARAM_NAME_GAP 6 // between a parameter's name and its slider
#define PARAM_TEXT_Y 4
#define VALUE_TEXT_SIZE 24
#define STEP_FOR_ONE_DECIMAL 0.1f
#define STEP_FOR_TWO_DECIMALS 0.01f
#define MAX_DECIMALS 3

// Parameter sliders.
#define SLIDER_TRACK_HEIGHT 3
#define SLIDER_TRACK_RADIUS 1.5f
#define DEFAULT_TICK_WIDTH 1
#define DEFAULT_TICK_HEIGHT 8
#define MIN_FILL_WIDTH 0.5f
#define KNOB_RADIUS 4
#define KNOB_RADIUS_SELECTED 5
#define DEPTH_DEFAULT_TICK 0.31f
#define DEPTH_SLIDER_FILL 0.32f
#define DEPTH_SLIDER_KNOB 0.33f

// Add page.
#define ADD_TILE_NAME_Y 6
#define ADD_TILE_TAG_Y 2
#define ADD_TILE_TAG_INSET 5
#define ADD_HELP_Y 166
#define ADD_HELP_LINES 3

// Save page.
#define SAVE_OPTION_TEXT_Y 7
#define SAVE_DETAIL_Y 10
#define SAVE_DETAIL_SIZE 64

// As many decimals as the parameter's step needs.
static int decimals_for_step(float step)
{
	if (step >= 1) {
		return 0;
	}
	if (step >= STEP_FOR_ONE_DECIMAL) {
		return 1;
	}
	if (step >= STEP_FOR_TWO_DECIMALS) {
		return 2;
	}
	return MAX_DECIMALS;
}

static void format_value(float value, float step, char *out, size_t outSize)
{
	snprintf(out, outSize, "%.*f", decimals_for_step(step), value);
}

static void draw_buttons(const char *const *labels, int buttonCount)
{
	float buttonWidth = ((float)PANEL_ROW_WIDTH - (buttonCount - 1) * TILE_GAP) / buttonCount;
	C2D_DrawRectSolid(0, EDITOR_BUTTON_Y - BUTTON_LINE_GAP, DEPTH_PANEL, BOTTOM_SCREEN_WIDTH, 1, COLOR_LINE);
	for (int i = 0; i < buttonCount; i++) {
		float x = PANEL_ROW_X + i * (buttonWidth + TILE_GAP);
		ui_shapes_round_rect(x, EDITOR_BUTTON_Y, buttonWidth, EDITOR_BUTTON_HEIGHT, TILE_RADIUS, COLOR_TILE,
		                     DEPTH_CONTROL);
		ui_text_draw_string(g_fontSmall, x + buttonWidth / 2, EDITOR_BUTTON_Y + BUTTON_TEXT_Y, 1.0f,
		                    COLOR_INK, TEXT_ALIGN_CENTER, buttonWidth - BUTTON_TEXT_PADDING, labels[i]);
	}
}

// The title bar says whether a test render is being made.
static void draw_header(const char *title)
{
	if (look_test_render_busy()) {
		ui_panel_draw_header('L', title, "Rendering...", COLOR_ACCENT);
	} else {
		ui_panel_draw_header('L', title, "SELECT test render", COLOR_FAINT);
	}
}

static void draw_step_row(const LookOp *step, int index, float y, bool selected)
{
	const LookOpInfo *info = &LOOK_OP_INFO[step->kind];
	if (selected) {
		ui_shapes_round_rect(PANEL_ROW_X, y, PANEL_ROW_WIDTH, STEP_ROW_BOX_HEIGHT, ROW_RADIUS,
		                     COLOR_TILE_SELECTED, DEPTH_PANEL);
		C2D_DrawRectSolid(PANEL_ROW_X, y + STEP_MARK_INSET, DEPTH_TILE, ROW_MARK_WIDTH,
		                  STEP_ROW_BOX_HEIGHT - 2 * STEP_MARK_INSET, COLOR_ACCENT);
	}
	ui_text_draw(g_fontSmall, STEP_NUMBER_X, y + STEP_SMALL_TEXT_Y, COLOR_FAINT, TEXT_ALIGN_CENTER, "%d",
	             index + 1);
	float nameWidth = ui_text_draw(g_fontBody, STEP_NAME_X, y + STEP_NAME_Y,
	                               selected ? COLOR_INK : COLOR_INK_SOFT, TEXT_ALIGN_LEFT, "%s", info->name);
	if (!look_op_runs_in_live_view(step->kind)) {
		ui_text_draw(g_fontSmall, STEP_NAME_X + nameWidth + STEP_TAG_GAP, y + STEP_SMALL_TEXT_Y, COLOR_FAINT,
		             TEXT_ALIGN_LEFT, "JPG");
	}
	char values[STEP_VALUES_SIZE];
	look_format_values(step, values, sizeof values, STEP_VALUES_ROOM);
	ui_text_draw_string(g_fontSmall, PANEL_VALUE_X, y + STEP_SMALL_TEXT_Y, 1.0f,
	                    selected ? COLOR_ACCENT : COLOR_MUTED, TEXT_ALIGN_RIGHT, ROW_VALUE_MAX_WIDTH, values);
}

static void draw_steps_page(void)
{
	const Look *look = &g_lookEditor.edited;
	draw_header("LOOK EDITOR");
	float nameWidth = ui_text_draw_string(g_fontBody, PANEL_TEXT_X, SUBHEADER_Y, 1.0f, COLOR_INK,
	                                      TEXT_ALIGN_LEFT, LOOK_NAME_MAX_WIDTH, look->name);
	if (g_lookEditor.unsavedChanges) {
		ui_shapes_circle(PANEL_TEXT_X + nameWidth + UNSAVED_DOT_GAP, UNSAVED_DOT_Y, UNSAVED_DOT_RADIUS,
		                 COLOR_ACCENT, DEPTH_CONTROL);
	}
	char fileText[LOOK_FILE_TEXT_SIZE];
	snprintf(fileText, sizeof fileText, "%s   %s", look->shortName, look->file);
	ui_text_draw_string(g_fontSmall, LOOK_FILE_RIGHT_X, LOOK_FILE_Y, 1.0f, COLOR_FAINT, TEXT_ALIGN_RIGHT,
	                    LOOK_FILE_MAX_WIDTH, fileText);
	C2D_DrawRectSolid(0, PANEL_SUBHEADER_LINE_Y, DEPTH_PANEL, BOTTOM_SCREEN_WIDTH, 1, COLOR_LINE);
	if (!look->opCount) {
		ui_text_draw(g_fontSmall, BOTTOM_CENTER_X, EMPTY_LIST_Y, COLOR_MUTED, TEXT_ALIGN_CENTER,
		             "No steps: X adds one (plain gamma 2.2 until then)");
	}
	int firstVisible = g_lookEditor.firstVisibleStep;
	for (int i = firstVisible; i < look->opCount && i < firstVisible + EDITOR_STEP_ROWS; i++) {
		float y = EDITOR_STEP_ROW_Y + (i - firstVisible) * EDITOR_STEP_ROW_HEIGHT;
		draw_step_row(&look->ops[i], i, y, i == g_lookEditor.stepSelection);
	}
	ui_panel_draw_scroll_marks(firstVisible, EDITOR_STEP_ROWS, look->opCount,
	                           EDITOR_STEP_ROW_Y - SCROLL_MARK_UP_GAP, EDITOR_STEP_LIST_BOTTOM);
	static const char *const buttonLabels[STEPS_BUTTON_COUNT] = {"X  Add", "Y  Remove", "L  Up", "R  Down",
	                                                             "START  Save"};
	draw_buttons(buttonLabels, STEPS_BUTTON_COUNT);
}

// The track, a tick at the default, a fill from zero to the value and the knob.
static void draw_param_slider(const LookOpInfo *info, float value, int param, float centerY, bool selected)
{
	float minValue = info->min[param];
	float maxValue = info->max[param];
	float span = maxValue > minValue ? maxValue - minValue : 1;
	float valuePosition = math_clamp_float((value - minValue) / span, 0, 1);
	float zeroPosition = math_clamp_float((0 - minValue) / span, 0, 1);
	float defaultPosition = (info->defaults[param] - minValue) / span;
	float trackWidth = EDITOR_SLIDER_WIDTH;
	float valueX = EDITOR_SLIDER_LEFT + valuePosition * trackWidth;
	float zeroX = EDITOR_SLIDER_LEFT + zeroPosition * trackWidth;
	float trackY = centerY - SLIDER_TRACK_HEIGHT / 2.0f;
	ui_shapes_round_rect(EDITOR_SLIDER_LEFT, trackY, trackWidth, SLIDER_TRACK_HEIGHT, SLIDER_TRACK_RADIUS,
	                     COLOR_LINE, DEPTH_CONTROL);
	if (defaultPosition >= 0 && defaultPosition <= 1) {
		float tickX = EDITOR_SLIDER_LEFT + defaultPosition * trackWidth - DEFAULT_TICK_WIDTH / 2.0f;
		C2D_DrawRectSolid(tickX, centerY - DEFAULT_TICK_HEIGHT / 2, DEPTH_DEFAULT_TICK, DEFAULT_TICK_WIDTH,
		                  DEFAULT_TICK_HEIGHT, COLOR_FAINT);
	}
	float fillLeft = zeroX < valueX ? zeroX : valueX;
	float fillRight = zeroX < valueX ? valueX : zeroX;
	if (fillRight - fillLeft > MIN_FILL_WIDTH) {
		ui_shapes_round_rect(fillLeft, trackY, fillRight - fillLeft, SLIDER_TRACK_HEIGHT, SLIDER_TRACK_RADIUS,
		                     selected ? COLOR_ACCENT : COLOR_MUTED, DEPTH_SLIDER_FILL);
	}
	ui_shapes_circle(valueX, centerY, selected ? KNOB_RADIUS_SELECTED : KNOB_RADIUS,
	                 selected ? COLOR_ACCENT : COLOR_INK, DEPTH_SLIDER_KNOB);
}

static void draw_param_row(const LookOp *step, int param, float y, bool selected)
{
	const LookOpInfo *info = &LOOK_OP_INFO[step->kind];
	float centerY = y + PARAM_ROW_BOX_HEIGHT / 2.0f;
	if (selected) {
		ui_shapes_round_rect(PANEL_ROW_X, y, PANEL_ROW_WIDTH, PARAM_ROW_BOX_HEIGHT, ROW_RADIUS,
		                     COLOR_TILE_SELECTED, DEPTH_PANEL);
	}
	const char *name = info->paramNames[param] ? info->paramNames[param] : "?";
	ui_text_draw_string(g_fontSmall, PARAM_NAME_X, y + PARAM_TEXT_Y, 1.0f, selected ? COLOR_INK : COLOR_MUTED,
	                    TEXT_ALIGN_LEFT, EDITOR_SLIDER_LEFT - PARAM_NAME_X - PARAM_NAME_GAP, name);
	draw_param_slider(info, step->values[param], param, centerY, selected);
	char valueText[VALUE_TEXT_SIZE];
	format_value(step->values[param], look_editor_param_step(info, param), valueText, sizeof valueText);
	ui_text_draw(g_fontSmall, PANEL_VALUE_X, y + PARAM_TEXT_Y, selected ? COLOR_ACCENT : COLOR_INK,
	             TEXT_ALIGN_RIGHT, "%s", valueText);
}

static void draw_params_page(void)
{
	const LookOp *step = &g_lookEditor.edited.ops[g_lookEditor.stepSelection];
	const LookOpInfo *info = &LOOK_OP_INFO[step->kind];
	char title[TITLE_SIZE];
	snprintf(title, sizeof title, "%s  >  STEP %d  %s", g_lookEditor.edited.name,
	         g_lookEditor.stepSelection + 1, info->name);
	draw_header(title);
	ui_text_draw_wrapped(g_fontSmall, PANEL_TEXT_X, SUBHEADER_Y, HELP_LINE_HEIGHT, HELP_LINE_CHARS,
	                     STEP_HELP_LINES, COLOR_INK_DIM, info->help);
	if (!look_op_runs_in_live_view(step->kind)) {
		ui_text_draw(g_fontSmall, PANEL_TEXT_X, LIVE_VIEW_NOTE_Y, COLOR_ACCENT, TEXT_ALIGN_LEFT,
		             "Not in the live view: SELECT renders it");
	}
	int firstVisible = g_lookEditor.firstVisibleParam;
	for (int j = firstVisible; j < step->count && j < firstVisible + EDITOR_PARAM_ROWS; j++) {
		float y = EDITOR_PARAM_ROW_Y + (j - firstVisible) * EDITOR_PARAM_ROW_HEIGHT;
		draw_param_row(step, j, y, j == g_lookEditor.paramSelection);
	}
	ui_panel_draw_scroll_marks(firstVisible, EDITOR_PARAM_ROWS, step->count,
	                           EDITOR_PARAM_ROW_Y - SCROLL_MARK_UP_GAP, EDITOR_PARAM_LIST_BOTTOM);
	static const char *const buttonLabels[PARAMS_BUTTON_COUNT] = {"Y  Default", "A  Next value", "B  Steps"};
	draw_buttons(buttonLabels, PARAMS_BUTTON_COUNT);
}

static void draw_add_page(void)
{
	draw_header("ADD A STEP");
	for (int kind = 0; kind < LOOK_OP_KINDS; kind++) {
		float x;
		float y;
		float width;
		float height;
		look_editor_add_tile_rect(kind, &x, &y, &width, &height);
		bool selected = kind == g_lookEditor.addSelection;
		if (selected) {
			ui_shapes_round_rect(x - SELECTION_OUTLINE, y - SELECTION_OUTLINE, width + 2 * SELECTION_OUTLINE,
			                     height + 2 * SELECTION_OUTLINE, SELECTION_RADIUS, COLOR_ACCENT, DEPTH_PANEL);
		}
		ui_shapes_round_rect(x, y, width, height, TILE_RADIUS, selected ? COLOR_TILE_SELECTED : COLOR_TILE,
		                     DEPTH_TILE);
		ui_text_draw(g_fontBody, x + width / 2, y + ADD_TILE_NAME_Y, selected ? COLOR_INK : COLOR_INK_SOFT,
		             TEXT_ALIGN_CENTER, "%s", LOOK_OP_INFO[kind].name);
		if (!look_op_runs_in_live_view((LookOpKind)kind)) {
			ui_text_draw(g_fontSmall, x + width - ADD_TILE_TAG_INSET, y + ADD_TILE_TAG_Y, COLOR_FAINT,
			             TEXT_ALIGN_RIGHT, "JPG");
		}
	}
	ui_text_draw_wrapped(g_fontSmall, PANEL_TEXT_X, ADD_HELP_Y, HELP_LINE_HEIGHT, HELP_LINE_CHARS,
	                     ADD_HELP_LINES, COLOR_INK_DIM, LOOK_OP_INFO[g_lookEditor.addSelection].help);
	static const char *const buttonLabels[ADD_BUTTON_COUNT] = {"A  Add after the selected step", "B  Back"};
	draw_buttons(buttonLabels, ADD_BUTTON_COUNT);
}

static u32 save_option_color(int option, bool selected)
{
	if (option == SAVE_OPTION_DISCARD) {
		return COLOR_RED;
	}
	return selected ? COLOR_INK : COLOR_INK_SOFT;
}

static void draw_save_page(void)
{
	draw_header("SAVE THE LOOK");
	static const char *const optionLabels[SAVE_OPTION_COUNT] = {"Save", "Save as a new look", "Rename",
	                                                            "Discard changes", "Keep editing"};
	char details[SAVE_OPTION_COUNT][SAVE_DETAIL_SIZE];
	int fileRoom = SAVE_DETAIL_SIZE - (int)sizeof "over ";
	snprintf(details[SAVE_OPTION_SAVE], SAVE_DETAIL_SIZE, "over %.*s", fileRoom, g_lookEditor.edited.file);
	snprintf(details[SAVE_OPTION_SAVE_AS_NEW], SAVE_DETAIL_SIZE, "user-NN.look; %s stays as it was",
	         g_lookEditor.saved.name);
	snprintf(details[SAVE_OPTION_RENAME], SAVE_DETAIL_SIZE, "%s  (%s)", g_lookEditor.edited.name,
	         g_lookEditor.edited.shortName);
	snprintf(details[SAVE_OPTION_DISCARD], SAVE_DETAIL_SIZE, "%s",
	         g_lookEditor.unsavedChanges ? "back to the version on the card" : "nothing changed");
	details[SAVE_OPTION_KEEP_EDITING][0] = 0;
	for (int option = 0; option < SAVE_OPTION_COUNT; option++) {
		float y = look_editor_save_row_y(option);
		bool selected = option == g_lookEditor.saveSelection;
		if (selected) {
			ui_shapes_round_rect(PANEL_ROW_X - SELECTION_OUTLINE, y - SELECTION_OUTLINE,
			                     PANEL_ROW_WIDTH + 2 * SELECTION_OUTLINE,
			                     EDITOR_SAVE_ROW_HEIGHT + 2 * SELECTION_OUTLINE, SELECTION_RADIUS,
			                     COLOR_ACCENT, DEPTH_PANEL);
		}
		ui_shapes_round_rect(PANEL_ROW_X, y, PANEL_ROW_WIDTH, EDITOR_SAVE_ROW_HEIGHT, TILE_RADIUS,
		                     selected ? COLOR_TILE_SELECTED : COLOR_TILE, DEPTH_TILE);
		ui_text_draw(g_fontBody, PANEL_ROW_TEXT_X, y + SAVE_OPTION_TEXT_Y,
		             save_option_color(option, selected), TEXT_ALIGN_LEFT, "%s", optionLabels[option]);
		ui_text_draw_string(g_fontSmall, PANEL_VALUE_X, y + SAVE_DETAIL_Y, 1.0f, COLOR_MUTED,
		                    TEXT_ALIGN_RIGHT, ROW_VALUE_MAX_WIDTH, details[option]);
	}
	C2D_DrawRectSolid(0, PANEL_FOOTER_LINE_Y, DEPTH_PANEL, BOTTOM_SCREEN_WIDTH, 1, COLOR_LINE);
	ui_text_draw(g_fontSmall, PANEL_TEXT_X, PANEL_FOOTER_TEXT_Y, COLOR_MUTED, TEXT_ALIGN_LEFT,
	             "A choose   B keep editing");
}

void look_editor_draw(void)
{
	switch (g_lookEditor.page) {
	case EDITOR_PAGE_PARAMS:
		draw_params_page();
		break;
	case EDITOR_PAGE_ADD:
		draw_add_page();
		break;
	case EDITOR_PAGE_SAVE:
		draw_save_page();
		break;
	default:
		draw_steps_page();
		break;
	}
}
