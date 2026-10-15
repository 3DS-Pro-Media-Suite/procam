#include "ui_panel.h"
#include <3ds.h>
#include <citro2d.h>
#include "ui_layout.h"
#include "ui_shapes.h"
#include "ui_text.h"

#define SCROLL_MARK_SIZE 2.5f
#define SCROLL_DOWN_MARK_HEIGHT 3.5f
#define TITLE_NOTE_GAP 6

void ui_panel_draw_header(char badge, const char *title, const char *note, u32 noteColor)
{
	ui_shapes_round_rect(PANEL_BADGE_X, PANEL_BADGE_Y, PANEL_BADGE_SIZE, PANEL_BADGE_SIZE, PANEL_BADGE_RADIUS,
	                     COLOR_ACCENT, DEPTH_PANEL);
	ui_text_draw(g_fontSmall, PANEL_BADGE_TEXT_X, PANEL_TITLE_Y, COLOR_BLACK, TEXT_ALIGN_CENTER, "%c", badge);
	float titleRightX = PANEL_NOTE_X;
	if (note) {
		float noteWidth = ui_text_draw(g_fontSmall, PANEL_NOTE_X, PANEL_TITLE_Y, noteColor, TEXT_ALIGN_RIGHT,
		                               "%s", note);
		titleRightX -= noteWidth + TITLE_NOTE_GAP;
	}
	// A long title, such as a long look name, shrinks to stay clear of the note.
	ui_text_draw_string(g_fontSmall, PANEL_TITLE_X, PANEL_TITLE_Y, 1.0f, COLOR_MUTED, TEXT_ALIGN_LEFT,
	                    titleRightX - PANEL_TITLE_X, title);
	C2D_DrawRectSolid(0, PANEL_HEADER_LINE_Y, DEPTH_PANEL, BOTTOM_SCREEN_WIDTH, 1, COLOR_LINE);
}

void ui_panel_draw_scroll_marks(int firstVisible, int visibleRows, int rowCount, float upMarkY,
                                float downMarkBaseY)
{
	if (firstVisible > 0) {
		ui_shapes_pointer(BOTTOM_CENTER_X, upMarkY, SCROLL_MARK_SIZE, POINTER_DIRECTION_UP, COLOR_MUTED,
		                  DEPTH_CONTROL);
	}
	if (firstVisible + visibleRows < rowCount) {
		ui_shapes_triangle(BOTTOM_CENTER_X, downMarkBaseY + SCROLL_DOWN_MARK_HEIGHT,
		                   BOTTOM_CENTER_X - SCROLL_MARK_SIZE, downMarkBaseY,
		                   BOTTOM_CENTER_X + SCROLL_MARK_SIZE, downMarkBaseY, COLOR_MUTED, DEPTH_CONTROL);
	}
}
