// Applies a changed setting or a menu action: step a row, run an action, swap screens.
#pragma once
#include <3ds.h>
#include "settings.h"

// Applies the side effects of a changed row: camera, stream, mode row and device status.
void setting_actions_on_change(const SettingRow *row);

// Steps a row by one value in `direction` (1 or -1) and applies it.
void setting_actions_step_row(const SettingRow *row, int direction);

void setting_actions_swap_screens(void);

// Runs a menu action. ACTION_EXIT sets *quit.
void setting_actions_run(RowAction action, bool *quit);
