// The look editor: edits a recipe step by step, saves it and handles its input. SELECT starts a test render
// (look_test_render.h); look_editor_draw.h draws it.
#pragma once
#include <3ds.h>

bool look_editor_active(void);
void look_editor_open(void);
void look_editor_input(u32 down, u32 held, touchPosition touch);
