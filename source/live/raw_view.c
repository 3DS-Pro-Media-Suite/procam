#include "raw_view.h"
#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include "cam_settings.h"
#include "cam_stream.h"
#include "look.h"
#include "look_render.h"
#include "math_util.h"
#include "sensor_color_matrix.h"
#include "settings.h"
#include "white_balance.h"

// Viewing raw frames. The raw data is the photo, saved untouched as DNG. This only makes it watchable: black
// level, white balance, the look, and each 2 x 2 RGGB cell shown as one colour on its four pixels.

#define RAW_LEVELS (RAW10_WHITE + 1)
#define LOOK_REVISION_NONE (~0u)
#define CELL_SIZE 2 // an RGGB cell is 2 x 2 pixels

// YUYV holds Y0 U Y1 V for each pair of pixels, full range.
#define YUYV_BYTES_PER_PIXEL 2
#define YUYV_PAIR_PIXELS 2
#define YUYV_PAIR_BYTES (YUYV_PAIR_PIXELS * YUYV_BYTES_PER_PIXEL)
#define YUYV_U 1
#define YUYV_V 3
#define CHROMA_ZERO 128

// BT.601 in fixed point: the weights are in 1/256.
#define WEIGHT_SHIFT 8
#define LUMA_RED_WEIGHT 77
#define LUMA_GREEN_WEIGHT 150
#define LUMA_BLUE_WEIGHT 29
#define V_TO_RED 359
#define U_TO_GREEN 88
#define V_TO_GREEN 183
#define U_TO_BLUE 454

#define RGB565_RED_SHIFT 11
#define RGB565_GREEN_SHIFT 5
#define FIVE_BIT_MASK 31
#define SIX_BIT_MASK 63

static LookState g_viewLook = {.look = -1}; // the live view's look
static LookLut g_viewLut = {.look = -1}; // the same look baked into a 3D table: the live view's fast path
_Atomic u32 g_lookRevision;
// Per channel: white-balance gain / (white - black), which turns a level above black into a linear level.
static float g_linearScale[RGB_CHANNELS];
// Per channel: raw sample -> index along that channel's axis of g_viewLut.
static u8 g_lutIndexOfSample[RGB_CHANNELS][RAW_LEVELS];

// The camera, matrix and look revisions the live view's tables hold.
static int g_matrixCamera = -1;
static u32 g_matrixRevision;
static u32 g_builtLookRevision = LOOK_REVISION_NONE;

void raw_view_black_levels(float levels[CFA_CELLS])
{
	for (int i = 0; i < CFA_CELLS; i++) {
		levels[i] = RAW10_BLACK;
	}
}

void raw_view_update_lut(void)
{
	int camera = math_clamp_int(g_settings.cam.camera, 0, CAM_COUNT - 1);
	u32 matrixRevision = sensor_color_matrix_revision();
	if (camera != g_matrixCamera || matrixRevision != g_matrixRevision) {
		g_matrixCamera = camera;
		g_matrixRevision = matrixRevision;
		look_render_set_color_matrix(sensor_color_matrix());
		// The look's table has the matrix baked in.
		g_builtLookRevision = LOOK_REVISION_NONE;
	}
	// RAW only has no look: the live view shows the standard one.
	bool rawOnly = g_settings.photoFormat == PHOTO_FORMAT_RAW_ONLY;
	int look = rawOnly ? LOOK_INDEX_STANDARD : g_settings.lookIndex;
	if (g_viewLook.look != look || g_builtLookRevision != g_lookRevision) {
		g_builtLookRevision = g_lookRevision;
		look_render_prepare(&g_viewLook, look);
		look_render_build_lut(&g_viewLut, look);
	}
	float gains[RGB_CHANNELS];
	white_balance_get(gains);
	for (int channel = 0; channel < RGB_CHANNELS; channel++) {
		g_linearScale[channel] = gains[channel] / (float)(RAW10_WHITE - RAW10_BLACK);
		for (int sample = 0; sample < RAW_LEVELS; sample++) {
			float linear = (sample - RAW10_BLACK) * g_linearScale[channel];
			g_lutIndexOfSample[channel][sample] = g_lookLutIndex[look_render_level_index(linear)];
		}
	}
}

static int luma_of(int red, int green, int blue)
{
	return (red * LUMA_RED_WEIGHT + green * LUMA_GREEN_WEIGHT + blue * LUMA_BLUE_WEIGHT) >> WEIGHT_SHIFT;
}

// The look's colour of one cell's raw samples. It points into the baked table, or into scratch while there
// is no table.
static const u8 *cell_color(int red, int green, int blue, u8 scratch[RGB_CHANNELS])
{
	if (!g_viewLut.rgb) {
		look_render_pixel(&g_viewLook, (red - RAW10_BLACK) * g_linearScale[RGB_RED],
		                  (green - RAW10_BLACK) * g_linearScale[RGB_GREEN],
		                  (blue - RAW10_BLACK) * g_linearScale[RGB_BLUE], scratch);
		return scratch;
	}
	size_t redIndex = g_lutIndexOfSample[RGB_RED][red];
	size_t greenIndex = g_lutIndexOfSample[RGB_GREEN][green];
	size_t blueIndex = g_lutIndexOfSample[RGB_BLUE][blue];
	size_t entry = (redIndex * LOOK_LUT_SIZE + greenIndex) * LOOK_LUT_SIZE + blueIndex;
	return g_viewLut.rgb + entry * RGB_CHANNELS;
}

// Both rows of a cell decode the same.
static void decode_raw_row(const u8 *frame, int frameWidth, int y, int firstX, int count, u8 *red, u8 *green,
                           u8 *blue, u8 *luma)
{
	int pairTop = y & ~1;
	const u8 *topRow = frame + (size_t)pairTop * frameWidth * RAW10_BYTES_PER_PIXEL;
	const u8 *bottomRow = topRow + (size_t)frameWidth * RAW10_BYTES_PER_PIXEL;
	// An odd firstX starts in the middle of a cell.
	for (int i = -(firstX & 1); i < count; i += CELL_SIZE) {
		const u8 *topCell = topRow + (size_t)(firstX + i) * RAW10_BYTES_PER_PIXEL;
		const u8 *bottomCell = bottomRow + (size_t)(firstX + i) * RAW10_BYTES_PER_PIXEL;
		int cellRed = raw10_unpack(topCell);
		int greenSum = raw10_unpack(topCell + RAW10_BYTES_PER_PIXEL) + raw10_unpack(bottomCell);
		int cellGreen = greenSum >> 1;
		int cellBlue = raw10_unpack(bottomCell + RAW10_BYTES_PER_PIXEL);
		u8 scratch[RGB_CHANNELS];
		const u8 *color = cell_color(cellRed, cellGreen, cellBlue, scratch);
		u8 viewRed = color[RGB_RED];
		u8 viewGreen = color[RGB_GREEN];
		u8 viewBlue = color[RGB_BLUE];
		u8 viewLuma = luma_of(viewRed, viewGreen, viewBlue);
		for (int k = 0; k < CELL_SIZE; k++) {
			int x = i + k;
			if (x < 0 || x >= count) {
				continue;
			}
			red[x] = viewRed;
			green[x] = viewGreen;
			blue[x] = viewBlue;
			luma[x] = viewLuma;
		}
	}
}

static void decode_yuyv_row(const u8 *frame, int frameWidth, int y, int firstX, int count, u8 *red, u8 *green,
                            u8 *blue, u8 *luma)
{
	const u8 *pair = frame + ((size_t)y * frameWidth + (firstX & ~1)) * YUYV_BYTES_PER_PIXEL;
	// An odd firstX starts in the middle of a pair.
	for (int i = -(firstX & 1); i < count; i += YUYV_PAIR_PIXELS) {
		int u = pair[YUYV_U] - CHROMA_ZERO;
		int v = pair[YUYV_V] - CHROMA_ZERO;
		// Full range.
		int redOffset = (V_TO_RED * v) >> WEIGHT_SHIFT;
		int greenOffset = (U_TO_GREEN * u + V_TO_GREEN * v) >> WEIGHT_SHIFT;
		int blueOffset = (U_TO_BLUE * u) >> WEIGHT_SHIFT;
		for (int k = 0; k < YUYV_PAIR_PIXELS; k++) {
			int x = i + k;
			if (x < 0 || x >= count) {
				continue;
			}
			int pixelLuma = pair[k * YUYV_BYTES_PER_PIXEL];
			red[x] = math_clamp_int(pixelLuma + redOffset, 0, 255);
			green[x] = math_clamp_int(pixelLuma - greenOffset, 0, 255);
			blue[x] = math_clamp_int(pixelLuma + blueOffset, 0, 255);
			luma[x] = pixelLuma;
		}
		pair += YUYV_PAIR_BYTES;
	}
}

// RGB565 fields widen to 8 bits by repeating their top bits below them.
static int widen_5_bits(int value)
{
	return (value << 3) | (value >> 2);
}

static int widen_6_bits(int value)
{
	return (value << 2) | (value >> 4);
}

static void decode_rgb565_row(const u16 *frame, int frameWidth, int y, int firstX, int count, u8 *red,
                              u8 *green, u8 *blue, u8 *luma)
{
	const u16 *row = frame + (size_t)y * frameWidth + firstX;
	for (int x = 0; x < count; x++) {
		u16 pixel = row[x];
		red[x] = widen_5_bits((pixel >> RGB565_RED_SHIFT) & FIVE_BIT_MASK);
		green[x] = widen_6_bits((pixel >> RGB565_GREEN_SHIFT) & SIX_BIT_MASK);
		blue[x] = widen_5_bits(pixel & FIVE_BIT_MASK);
		luma[x] = luma_of(red[x], green[x], blue[x]);
	}
}

void raw_view_decode_row(const void *frame, int format, int frameWidth, int y, int firstX, int count, u8 *red,
                         u8 *green, u8 *blue, u8 *luma)
{
	if (format == CAM_FORMAT_RAW) {
		decode_raw_row(frame, frameWidth, y, firstX, count, red, green, blue, luma);
		return;
	}
	if (format == CAM_FORMAT_RGB565) {
		decode_rgb565_row(frame, frameWidth, y, firstX, count, red, green, blue, luma);
		return;
	}
	decode_yuyv_row(frame, frameWidth, y, firstX, count, red, green, blue, luma);
}

u8 *raw_view_frame_rgb(const void *frame, int format, int frameWidth, int left, int top, int width,
                       int height)
{
	u8 *rgb = malloc((size_t)width * height * RGB_CHANNELS);
	if (!rgb) {
		return NULL;
	}
	DecodedRow row;
	for (int y = 0; y < height; y++) {
		raw_view_decode_row(frame, format, frameWidth, top + y, left, width, row.red, row.green, row.blue,
		                    row.luma);
		u8 *destination = rgb + (size_t)y * width * RGB_CHANNELS;
		for (int x = 0; x < width; x++) {
			destination[x * RGB_CHANNELS + RGB_RED] = row.red[x];
			destination[x * RGB_CHANNELS + RGB_GREEN] = row.green[x];
			destination[x * RGB_CHANNELS + RGB_BLUE] = row.blue[x];
		}
	}
	return rgb;
}
