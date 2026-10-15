// The look editor's test render: the live view's last raw frame through every step of a look, made on a
// worker thread and shown on the top screen for a while.
#pragma once
#include <3ds.h>

// Starts a render through the look. Tells the user when there is no raw frame or not enough memory.
void look_test_render_start(int lookIndex);
// Main loop: shows a finished render. Returns true while one is being made.
bool look_test_render_poll(void);
bool look_test_render_busy(void);
