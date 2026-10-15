// Screen sizes and the layout the bottom-screen panels share: title bar, margins, rows, tiles, footer and the
// drawing depths.
#pragma once

#define TOP_SCREEN_WIDTH 400
#define BOTTOM_SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240
#define TOP_CENTER_X (TOP_SCREEN_WIDTH / 2)
#define BOTTOM_CENTER_X (BOTTOM_SCREEN_WIDTH / 2)
#define SCREEN_CENTER_Y (SCREEN_HEIGHT / 2)

// Title bar: a letter on an accent badge, the title, a note on the right and a line below.
#define PANEL_BADGE_X 6
#define PANEL_BADGE_Y 3
#define PANEL_BADGE_SIZE 16
#define PANEL_BADGE_RADIUS 3
#define PANEL_BADGE_TEXT_X (PANEL_BADGE_X + PANEL_BADGE_SIZE / 2)
#define PANEL_TITLE_X 28
#define PANEL_TITLE_Y 5
#define PANEL_NOTE_X 314
#define PANEL_HEADER_LINE_Y 21
#define PANEL_SUBHEADER_LINE_Y 45 // under a second header row: the setup tabs, the look's name
#define PANEL_CONTENT_Y 28 // the first tile row under the title bar
#define PANEL_FOOTER_LINE_Y 206
#define PANEL_FOOTER_TEXT_Y 214

#define PANEL_TEXT_X 10
#define PANEL_ROW_X 6
#define PANEL_ROW_WIDTH (BOTTOM_SCREEN_WIDTH - 2 * PANEL_ROW_X)
#define PANEL_ROW_TEXT_X 16
#define PANEL_VALUE_X 306 // right edge of a row's value
#define ROW_RADIUS 5
#define ROW_MARK_WIDTH 2 // the accent bar beside a selected row
#define TILE_RADIUS 6
#define TILE_GAP 4
#define SELECTION_OUTLINE 1.5f // the accent ring around a selected tile
#define SELECTION_RADIUS 7

// Drawing depths: a larger value is drawn on top.
#define DEPTH_PANEL 0.2f
#define DEPTH_TILE 0.21f
#define DEPTH_CONTROL 0.3f
