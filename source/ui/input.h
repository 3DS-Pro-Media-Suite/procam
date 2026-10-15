// Buttons, sticks and touch for the shooting screen and the menus.
#pragma once
#include <3ds.h>

// True while SELECT is held long enough to show the button hints.
extern bool g_showControls;

// Handles one frame of input. Sets *quit when the app should exit.
void input_handle(bool *quit);
