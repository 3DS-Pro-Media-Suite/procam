// The live view's meter: the mean green level above black in the metering area of each live raw frame, for
// the raw exposure loop. The area is a touched spot, or else the Metering setting's window.
#pragma once
#include <3ds.h>
#include "raw_grid.h"

#define METERING_TOUCH_SPOT_DIVISOR 10 // the touch spot reaches a tenth of the frame to each side

// Meters a live raw frame (preview worker).
void metering_update(const u8 *frame, int width, int height, const RawGrid *grid);
// The metered level. Negative while nothing is metered yet.
float metering_green(void);
// Frames metered so far.
u32 metering_frames(void);

// The touch metering point, 0..1 of the frame.
void metering_set_touch_point(float x, float y);
void metering_clear_touch_point(void);
// False while there is none.
bool metering_touch_point(float *x, float *y);
