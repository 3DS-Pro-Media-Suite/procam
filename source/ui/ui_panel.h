// The title bar and the scroll marks of the bottom-screen panels.
#pragma once
#include <3ds.h>

// A letter on an accent badge, the title, a note on the right (NULL: none) and a line below.
void ui_panel_draw_header(char badge, const char *title, const char *note, u32 noteColor);

// Marks for a list that shows rowCount rows from firstVisible, visibleRows at a time: a pointer at upMarkY
// while rows are hidden above it, a triangle with its base at downMarkBaseY while rows are hidden below.
void ui_panel_draw_scroll_marks(int firstVisible, int visibleRows, int rowCount, float upMarkY,
                                float downMarkBaseY);
