// The self-timer: a shutter press starts it, the shot comes when it runs out.
#pragma once
#include <3ds.h>

// Starts or cancels the self-timer, or asks for the shot at once when it is off.
void self_timer_press(void);
// Main loop: asks for the shot when the time is up.
void self_timer_poll(void);
bool self_timer_counting(void);
// The count runs from *start to *end, in osGetTime milliseconds.
void self_timer_progress(u64 *start, u64 *end);
