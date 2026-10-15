// Battery level, free card space and the battery icon.
#pragma once
#include <3ds.h>

// Main loop: reads the battery every few seconds, or now when force is set. The free card space is read on a
// background thread while the card is idle.
void device_status_refresh(bool force);
// The battery icon with its top left corner at x, y.
void device_status_draw_battery(float x, float y, float z);
