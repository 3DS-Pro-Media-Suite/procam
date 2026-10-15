// Renders pixels through a look: the live view's tables, the JPG render and the raw hot pixel fix.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "raw10.h"

// Entries of the tone table, which maps a linear level in 0..1 to a display byte.
#define LOOK_TONE_TABLE_SIZE 4096
// Steps of the curve table over display levels 0..1. The table holds one entry more.
#define LOOK_CURVE_STEPS 256

// Entries per axis of the live view's 3D table.
#define LOOK_LUT_SIZE 48
// Linear levels that g_lookLutIndex resolves.
#define LOOK_LUT_LEVELS 1024
#define LOOK_LUT_LEVEL_MAX (LOOK_LUT_LEVELS - 1)

// Sets the camera's colour matrix: white-balanced camera RGB to linear sRGB, row-major, rows summing to 1. It
// runs before every step of every look. NULL removes it.
void look_render_set_color_matrix(const float matrix[COLOR_MATRIX_ENTRIES]);

// Tables for rendering single pixels. Only the per-pixel steps run; raw-stage and spatial steps are skipped.
typedef struct {
	int look;
	uint8_t toneTable[LOOK_TONE_TABLE_SIZE];
	float curveTable[LOOK_CURVE_STEPS + 1];
	bool hasTone;
	bool hasCurve;
} LookState;

void look_render_prepare(LookState *state, int lookIndex);
// Linear, white-balanced RGB to 8-bit display RGB.
void look_render_pixel(const LookState *state, float red, float green, float blue, uint8_t out[RGB_CHANNELS]);

// The look's per-pixel steps baked into a 3D table over linear, white-balanced RGB. The axes are square-root
// spaced. A lookup takes the nearest entry.
typedef struct {
	int look;
	uint8_t *rgb; // LOOK_LUT_SIZE^3 RGB entries, allocated by look_render_build_lut
} LookLut;

bool look_render_build_lut(LookLut *lut, int lookIndex);
// Table axis index for each linear level (level x LOOK_LUT_LEVEL_MAX). Filled by look_render_build_lut.
extern uint8_t g_lookLutIndex[LOOK_LUT_LEVELS];

// The g_lookLutIndex slot for a linear level.
static inline int look_render_level_index(float level)
{
	if (level <= 0) {
		return 0;
	}
	if (level >= 1) {
		return LOOK_LUT_LEVEL_MAX;
	}
	return (int)(level * LOOK_LUT_LEVEL_MAX);
}

// The JPG render: a raw RGGB frame of 8+2 samples, its black and white levels and white-balance gains to
// 8-bit RGB (width x height), with every step. Needs width x height x 12 bytes of heap. Returns false when
// out of memory.
bool look_render(const uint8_t *raw, int width, int height, int black, int white,
                 const float whiteBalance[RGB_CHANNELS], int lookIndex, uint8_t *rgb);
// The same from 16-bit RGGB samples, which are not modified: a merged raw or a DNG's samples. An HDR raw's
// own white lies `range` times above the display white. Its highlights above half the display white roll off
// smoothly instead of clipping and keep their colour. range <= 1 renders a plain 16-bit raw. The recipe's
// hot pixel threshold is in 10-bit units and scales by the display white / RAW10_WHITE.
bool look_render_hdr(const uint16_t *raw, int width, int height, int black, int white, float range,
                     const float whiteBalance[RGB_CHANNELS], int lookIndex, uint8_t *rgb);

// Fixes hot and dead pixels of a raw 8+2 frame in place, before the DNG is written. map holds mapCount
// (x, y) pairs in the full 640 x 480 frame; offsetX and offsetY place this frame in it. With threshold > 0,
// any other pixel that stands out by more than threshold is fixed too. Returns the number of pixels replaced.
int look_render_fix_hot_pixels(uint8_t *raw, int width, int height, int offsetX, int offsetY,
                               const uint16_t *map, int mapCount, int threshold);
