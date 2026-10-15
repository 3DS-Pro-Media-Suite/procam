// The look editor's state and layout, shared by its input (look_editor.c) and its drawing
// (look_editor_draw.c).
#pragma once
#include <3ds.h>
#include "look.h"

typedef enum {
	EDITOR_PAGE_OFF,
	EDITOR_PAGE_STEPS,
	EDITOR_PAGE_PARAMS,
	EDITOR_PAGE_ADD,
	EDITOR_PAGE_SAVE
} LookEditorPage;

// The choices on the save page, top to bottom.
typedef enum {
	SAVE_OPTION_SAVE,
	SAVE_OPTION_SAVE_AS_NEW,
	SAVE_OPTION_RENAME,
	SAVE_OPTION_DISCARD,
	SAVE_OPTION_KEEP_EDITING,
	SAVE_OPTION_COUNT
} LookEditorSaveOption;

// The buttons along the bottom of each page, left to right.
typedef enum {
	STEPS_BUTTON_ADD,
	STEPS_BUTTON_REMOVE,
	STEPS_BUTTON_UP,
	STEPS_BUTTON_DOWN,
	STEPS_BUTTON_SAVE,
	STEPS_BUTTON_COUNT
} LookEditorStepsButton;

typedef enum {
	PARAMS_BUTTON_DEFAULT,
	PARAMS_BUTTON_NEXT,
	PARAMS_BUTTON_BACK,
	PARAMS_BUTTON_COUNT
} LookEditorParamsButton;

typedef enum {
	ADD_BUTTON_ADD,
	ADD_BUTTON_BACK,
	ADD_BUTTON_COUNT
} LookEditorAddButton;

typedef struct {
	LookEditorPage page;
	int lookIndex; // the look being edited
	Look edited; // every change to it goes live at once
	Look saved; // the recipe as it is on the card
	bool unsavedChanges;
	int stepSelection;
	int firstVisibleStep;
	int paramSelection;
	int firstVisibleParam;
	int addSelection;
	int saveSelection;
	int draggedParam; // the parameter whose slider is being dragged, -1 for none
} LookEditor;

#define EDITOR_STEP_ROWS 7 // steps shown at once
#define EDITOR_STEP_ROW_Y 49
#define EDITOR_STEP_ROW_HEIGHT 22
#define EDITOR_STEP_LIST_BOTTOM (EDITOR_STEP_ROW_Y + EDITOR_STEP_ROWS * EDITOR_STEP_ROW_HEIGHT)
#define EDITOR_PARAM_ROWS 7 // parameters shown at once
#define EDITOR_PARAM_ROW_Y 64
#define EDITOR_PARAM_ROW_HEIGHT 20
#define EDITOR_PARAM_LIST_BOTTOM (EDITOR_PARAM_ROW_Y + EDITOR_PARAM_ROWS * EDITOR_PARAM_ROW_HEIGHT)
#define EDITOR_SLIDER_LEFT 96
#define EDITOR_SLIDER_RIGHT 250
#define EDITOR_SLIDER_WIDTH (EDITOR_SLIDER_RIGHT - EDITOR_SLIDER_LEFT)
#define EDITOR_SAVE_ROW_HEIGHT 30
#define EDITOR_BUTTON_Y 211
#define EDITOR_BUTTON_HEIGHT 25

extern LookEditor g_lookEditor;

// How much one press changes a parameter.
float look_editor_param_step(const LookOpInfo *info, int param);
// Where the add page shows the tile of one step kind.
void look_editor_add_tile_rect(int kind, float *x, float *y, float *width, float *height);
float look_editor_save_row_y(int option);
