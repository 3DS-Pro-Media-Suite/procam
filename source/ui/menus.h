// The bottom screen panels: the Q menu, the setup menu and the camera information page.
#pragma once
#include <3ds.h>
#include "settings.h"

// The quick menu grid.
#define QUICK_TILE_WIDTH 74
#define QUICK_TILE_HEIGHT 40
#define QUICK_GRID_COLUMNS 4
#define QUICK_GRID_ROWS (QUICK_TILE_COUNT / QUICK_GRID_COLUMNS)

// The setup menu: tabs, then the rows.
#define MENU_VISIBLE_ROWS 6
#define MENU_TAB_WIDTH 80
#define MENU_ROW_Y 50
#define MENU_ROW_HEIGHT 25

// Buttons that are drawn here and tapped on the touch screen.
#define ARROW_LEFT_X 204
#define ARROW_RIGHT_X 240
#define ARROW_Y 206
#define ARROW_WIDTH 32
#define ARROW_HEIGHT 28
#define SHUTTER_BUTTON_X 293
#define SHUTTER_BUTTON_Y 220
#define GALLERY_BUTTON_X 196
#define GALLERY_BUTTON_Y 2
#define GALLERY_BUTTON_WIDTH 62
#define GALLERY_BUTTON_HEIGHT 17

extern int g_quickSelection;
extern bool g_menuOpen;
extern bool g_infoOpen;

// Top left corner of a quick menu tile.
void menus_quick_tile_rect(int tile, float *x, float *y);

// Draws the bottom screen: the gallery, the look editor, the value picker or one of the menus.
void menus_draw(void);

// A tap on the shutter button.
bool menus_touch_on_shutter(touchPosition touch);

// One frame of input while the setup menu is open. ACTION_EXIT sets *quit.
void menus_setup_input(u32 down, u32 held, touchPosition touch, bool *quit);
