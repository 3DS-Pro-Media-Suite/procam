// The live view's auto white balance: viewing gains estimated from the raw grid of each live frame. They are
// the DNG's as-shot neutral and the JPG's white balance too.
#pragma once
#include <3ds.h>
#include "raw10.h"
#include "raw_grid.h"

// Moves the gains towards this frame's estimate (preview worker).
void white_balance_update(const RawGrid *grid);
// The current gains, red, green and blue.
void white_balance_get(float gains[RGB_CHANNELS]);
