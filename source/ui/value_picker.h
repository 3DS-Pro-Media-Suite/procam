// The value picker: every value of one setting at once, opened by tapping a selected tile or menu row again
// (or A in the setup menu). Tap a value to choose it; B or a tap outside closes it.
#pragma once
#include <3ds.h>
#include "settings.h"

bool value_picker_active(void);
void value_picker_close(void);

int value_picker_count(const SettingRow *row);

// The rectangle of value number index on the bottom screen.
void value_picker_rect(const SettingRow *row, int index, float *x, float *y, float *width, float *height);

// Opens the picker, or shows why the setting cannot change now.
void value_picker_open(const SettingRow *row);

void value_picker_draw(void);

// Chooses value number index and closes the picker. A value that cannot be chosen now keeps it open.
void value_picker_choose(int index);

// One frame of input while the picker is open.
void value_picker_input(u32 down, u32 held, touchPosition touch);
