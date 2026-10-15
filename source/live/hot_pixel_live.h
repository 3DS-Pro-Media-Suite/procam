// The live view's part of the hot pixel learner: live raw frames teach it, the pixels it learned are hidden
// in them, and its counts go to the card now and then.
#pragma once
#include <3ds.h>

// Feeds a live raw frame to the learner and patches the learned pixels in it (preview worker).
void hot_pixel_live_update(u8 *frame, int width, int height);

// Main loop: saves the counts every few minutes, when nothing else is writing.
void hot_pixel_live_autosave_poll(u64 nowMs);
