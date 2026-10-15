// Camera frames as displayable colours: raw levels, viewing white balance, the sensor colour matrix and
// the look table.
#pragma once
#include <3ds.h>
#include "math_util.h"
#include "raw10.h"

// The 10-bit sample at x, y of a raw frame frameWidth pixels wide.
static inline int raw_view_sample(const u8 *frame, int frameWidth, int y, int x)
{
	return raw10_unpack(frame + ((size_t)y * frameWidth + x) * RAW10_BYTES_PER_PIXEL);
}

// A sample's level above black.
static inline int raw_view_above_black(int sample)
{
	return math_clamp_int(sample - RAW10_BLACK, 0, RAW10_WHITE);
}

#define RAW_VIEW_MAX_WIDTH 640

// One decoded row. Every thread uses its own.
typedef struct {
	u8 red[RAW_VIEW_MAX_WIDTH];
	u8 green[RAW_VIEW_MAX_WIDTH];
	u8 blue[RAW_VIEW_MAX_WIDTH];
	u8 luma[RAW_VIEW_MAX_WIDTH + 1]; // one spare entry: the edge test reads one pixel past the row
} DecodedRow;

extern _Atomic u32 g_lookRevision; // bumped when a look changes, so the live view rebuilds its tables

void raw_view_black_levels(float levels[CFA_CELLS]);

// Rebuilds the live view's tables when the camera, the look or the white balance changed.
void raw_view_update_lut(void);

// One row of a camera frame as 8-bit red, green, blue and luma, count pixels from firstX. format is a
// CAM_FORMAT_*. A raw row pair decodes to one row of cells: each cell's colour on all four of its pixels.
void raw_view_decode_row(const void *frame, int format, int frameWidth, int y, int firstX, int count, u8 *red,
                         u8 *green, u8 *blue, u8 *luma);

// A region of a camera frame as top-first RGB, malloc'd. NULL when out of memory.
u8 *raw_view_frame_rgb(const void *frame, int format, int frameWidth, int left, int top, int width,
                       int height);
