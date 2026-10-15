#include "look_render.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "look.h"
#include "math_util.h"
#include "raw10.h"

#define TONE_TABLE_LAST (LOOK_TONE_TABLE_SIZE - 1)
#define DISPLAY_BYTE_MAX 255.0f // display level 1 as a byte

// Luma weights of red, green and blue.
#define REC709_RED 0.2126f
#define REC709_GREEN 0.7152f
#define REC709_BLUE 0.0722f
#define REC601_RED 0.299f
#define REC601_GREEN 0.587f
#define REC601_BLUE 0.114f
#define DEFAULT_GAMMA 2.2f
// A tone step's gamma at or below this counts as not given.
#define MIN_GAMMA 0.5f

// The values of a tone step.
enum {
	TONE_CONTRAST,
	TONE_LIFT,
	TONE_ROLLOFF,
	TONE_GAMMA,
	TONE_VALUE_COUNT
};

// A curve step's values are (x, y) pairs.
#define CURVE_POINT_VALUES 2
#define MIN_CURVE_POINTS 2

// A tint value of 0.25 adds a full level at pure black or white, so the recipes' tints stay small numbers.
#define SPLIT_TONE_GAIN 4

#define LUT_LAST_ENTRY (LOOK_LUT_SIZE - 1)
#define LUT_BYTES ((size_t)LOOK_LUT_SIZE * LOOK_LUT_SIZE * LOOK_LUT_SIZE * RGB_CHANNELS)

// Same-colour Bayer neighbours lie up to two pixels away, so pixels closer to the edge are not fixed.
#define EDGE_MARGIN 2
#define RED_BLUE_NEIGHBOR_COUNT 4
#define GREEN_NEIGHBOR_COUNT 8
// Start values for a search of the lowest and highest sample.
#define ABOVE_ANY_SAMPLE (1 << 20)
#define BELOW_ANY_SAMPLE (-1)

// A range of 1: the raw's white is the display white.
#define STANDARD_RANGE 1.0f
// HDR highlights roll off above this linear level.
#define HDR_KNEE 0.5f

#define SHARPEN_RADIUS 1
#define SHARPEN_BOX_PIXELS ((2 * SHARPEN_RADIUS + 1) * (2 * SHARPEN_RADIUS + 1))

// Grain comes from a linear congruential generator with a fixed seed, so a frame always gets the same grain.
#define GRAIN_SEED 0x9E3779B9u
#define LCG_MULTIPLIER 1664525u
#define LCG_INCREMENT 1013904223u
#define NOISE_BITS 23
#define NOISE_SHIFT (32 - NOISE_BITS)
#define NOISE_SCALE ((float)(1u << NOISE_BITS))

typedef struct {
	int dy;
	int dx;
} PixelOffset;

// Two pixels away on each axis, then for greens also the four diagonal greens.
static const PixelOffset SAME_COLOR_NEIGHBORS[GREEN_NEIGHBOR_COUNT] = {
        {0, -2}, {0, 2}, {-2, 0}, {2, 0}, {-1, -1}, {-1, 1}, {1, -1}, {1, 1},
};

typedef struct {
	const uint16_t *samples;
	int width;
	int height;
	int black;
} BayerFrame;

static float g_colorMatrix[COLOR_MATRIX_ENTRIES];
static bool g_hasColorMatrix;

// Table index for each linear level: square-root spaced, so the shadows get most of the entries.
uint8_t g_lookLutIndex[LOOK_LUT_LEVELS];

// A display level in 0..1 rounded to a byte.
static uint8_t to_byte(float level)
{
	return (uint8_t)(math_clamp_float(level, 0, 1) * DISPLAY_BYTE_MAX + 0.5f);
}

static void scale_rgb(float rgb[RGB_CHANNELS], float factor)
{
	rgb[0] *= factor;
	rgb[1] *= factor;
	rgb[2] *= factor;
}

// Rec. 709 luma weights.
static float luma_rec709(const float rgb[RGB_CHANNELS])
{
	return REC709_RED * rgb[0] + REC709_GREEN * rgb[1] + REC709_BLUE * rgb[2];
}

// Rec. 601 luma weights.
static float luma_rec601(const float rgb[RGB_CHANNELS])
{
	return REC601_RED * rgb[0] + REC601_GREEN * rgb[1] + REC601_BLUE * rgb[2];
}

// Gamma, then contrast as a blend towards a smoothstep S-curve, a highlight roll-off and a lift of blacks.
static void build_tone_table(uint8_t *table, const float *values)
{
	float contrast = values[TONE_CONTRAST];
	float lift = values[TONE_LIFT];
	float rolloff = values[TONE_ROLLOFF];
	float gamma = values[TONE_GAMMA] > MIN_GAMMA ? values[TONE_GAMMA] : DEFAULT_GAMMA;
	for (int i = 0; i < LOOK_TONE_TABLE_SIZE; i++) {
		float linear = i / (float)TONE_TABLE_LAST;
		float level = powf(linear, 1 / gamma);
		float sCurve = level * level * (3 - 2 * level);
		level = level + contrast * (sCurve - level);
		level = level * (1 + rolloff) / (1 + rolloff * level * level);
		level = lift + level * (1 - lift);
		table[i] = to_byte(level);
	}
}

// A piecewise linear curve through the step's (x, y) points, in rising x. Before the first point it holds the
// first y, after the last point the last y. With fewer than two points it is the identity.
static void build_curve_table(float *table, const LookOp *step)
{
	int pointCount = step->count / CURVE_POINT_VALUES;
	for (int i = 0; i <= LOOK_CURVE_STEPS; i++) {
		float x = i / (float)LOOK_CURVE_STEPS;
		float y = x;
		if (pointCount >= MIN_CURVE_POINTS) {
			y = step->values[1];
			for (int k = 0; k + 1 < pointCount; k++) {
				int first = k * CURVE_POINT_VALUES;
				float x0 = step->values[first];
				float y0 = step->values[first + 1];
				float x1 = step->values[first + 2];
				float y1 = step->values[first + 3];
				if (x >= x0 && x <= x1 && x1 > x0) {
					y = y0 + (y1 - y0) * (x - x0) / (x1 - x0);
					break;
				}
				if (x > x1) {
					y = y1;
				}
			}
		}
		table[i] = math_clamp_float(y, 0, 1);
	}
}

void look_render_prepare(LookState *state, int lookIndex)
{
	const Look *look = look_get(lookIndex);
	state->look = lookIndex;
	state->hasTone = false;
	state->hasCurve = false;
	for (int k = 0; k < look->opCount; k++) {
		if (look->ops[k].kind == LOOK_OP_TONE && !state->hasTone) {
			build_tone_table(state->toneTable, look->ops[k].values);
			state->hasTone = true;
		}
		if (look->ops[k].kind == LOOK_OP_CURVE && !state->hasCurve) {
			build_curve_table(state->curveTable, &look->ops[k]);
			state->hasCurve = true;
		}
	}
	// A look without a tone step ends in plain gamma: no contrast, lift or roll-off.
	if (!state->hasTone) {
		float plainGamma[TONE_VALUE_COUNT] = {0, 0, 0, DEFAULT_GAMMA};
		build_tone_table(state->toneTable, plainGamma);
	}
}

static inline int tone_index(float level)
{
	int index;
	if (level <= 0) {
		index = 0;
	} else if (level >= 1) {
		index = TONE_TABLE_LAST;
	} else {
		index = (int)(level * TONE_TABLE_LAST + 0.5f);
	}
	return index;
}

void look_render_set_color_matrix(const float matrix[COLOR_MATRIX_ENTRIES])
{
	if (matrix) {
		memcpy(g_colorMatrix, matrix, sizeof g_colorMatrix);
	}
	g_hasColorMatrix = matrix != NULL;
}

// rgb becomes the 3 x 3 row-major matrix times (red, green, blue).
static void multiply_matrix(const float *matrix, float red, float green, float blue, float rgb[RGB_CHANNELS])
{
	rgb[0] = matrix[0] * red + matrix[1] * green + matrix[2] * blue;
	rgb[1] = matrix[3] * red + matrix[4] * green + matrix[5] * blue;
	rgb[2] = matrix[6] * red + matrix[7] * green + matrix[8] * blue;
}

// Channels are clipped to 1 first: above it, a blown highlight turns magenta through the matrix.
static void apply_color_matrix(float rgb[RGB_CHANNELS])
{
	float red = rgb[0] > 1 ? 1 : rgb[0];
	float green = rgb[1] > 1 ? 1 : rgb[1];
	float blue = rgb[2] > 1 ? 1 : rgb[2];
	multiply_matrix(g_colorMatrix, red, green, blue, rgb);
}

// values[0] is the saturation: 0 gives grey, 1 keeps the colour.
static void apply_saturation(float rgb[RGB_CHANNELS], const float *values)
{
	float luma = luma_rec709(rgb);
	for (int i = 0; i < RGB_CHANNELS; i++) {
		rgb[i] = luma + (rgb[i] - luma) * values[0];
	}
}

// Grey from the weighted channels. The weights are normalised.
static void apply_mono(float rgb[RGB_CHANNELS], const float weights[RGB_CHANNELS])
{
	float weightSum = weights[0] + weights[1] + weights[2];
	float grey = 0;
	if (weightSum > 0) {
		grey = (weights[0] * rgb[0] + weights[1] * rgb[1] + weights[2] * rgb[2]) / weightSum;
	}
	rgb[0] = grey;
	rgb[1] = grey;
	rgb[2] = grey;
}

// Linear levels to display levels.
static inline void apply_tone_table(const LookState *state, float rgb[RGB_CHANNELS])
{
	for (int i = 0; i < RGB_CHANNELS; i++) {
		rgb[i] = state->toneTable[tone_index(rgb[i])] / DISPLAY_BYTE_MAX;
	}
}

static void apply_curve_table(const LookState *state, float rgb[RGB_CHANNELS])
{
	for (int i = 0; i < RGB_CHANNELS; i++) {
		int entry = (int)(math_clamp_float(rgb[i], 0, 1) * LOOK_CURVE_STEPS + 0.5f);
		rgb[i] = state->curveTable[entry];
	}
}

// The shadow tint goes where the pixel is dark, the highlight tint where it is bright.
static void apply_split_tone(float rgb[RGB_CHANNELS], const float *values)
{
	const float *shadowTint = values;
	const float *highlightTint = values + LOOK_SPLIT_TONE_SHADOW_VALUES;
	float luma = math_clamp_float(luma_rec601(rgb), 0, 1);
	float shadowWeight = (1 - luma) * (1 - luma);
	float highlightWeight = luma * luma;
	for (int i = 0; i < RGB_CHANNELS; i++) {
		float shadow = shadowWeight * shadowTint[i] * SPLIT_TONE_GAIN;
		float highlight = highlightWeight * highlightTint[i] * SPLIT_TONE_GAIN;
		rgb[i] += shadow + highlight;
	}
}

// One pixel through the per-pixel steps: linear RGB in, display RGB (0..1) out. vignetteDistance is the
// squared distance from the centre, 0 at the centre and about 1 in the corners.
static void render_pixel(const Look *look, const LookState *state, float rgb[RGB_CHANNELS],
                         float vignetteDistance)
{
	if (g_hasColorMatrix) {
		apply_color_matrix(rgb);
	}
	// The first tone or curve step moves the pixel to display levels.
	bool isDisplay = false;
	for (int k = 0; k < look->opCount; k++) {
		const LookOp *step = &look->ops[k];
		const float *values = step->values;
		switch (step->kind) {
		case LOOK_OP_EXPOSURE:
			scale_rgb(rgb, powf(2, values[0]));
			break;
		case LOOK_OP_WB_SHIFT:
			// Extra red and blue gains.
			rgb[0] *= values[0];
			rgb[2] *= values[1];
			break;
		case LOOK_OP_MATRIX:
			multiply_matrix(values, rgb[0], rgb[1], rgb[2], rgb);
			break;
		case LOOK_OP_SATURATION:
			apply_saturation(rgb, values);
			break;
		case LOOK_OP_MONO:
			apply_mono(rgb, values);
			break;
		case LOOK_OP_TONE:
			if (!isDisplay) {
				apply_tone_table(state, rgb);
				isDisplay = true;
			}
			break;
		case LOOK_OP_CURVE:
			if (!isDisplay) {
				apply_tone_table(state, rgb);
				isDisplay = true;
			}
			apply_curve_table(state, rgb);
			break;
		case LOOK_OP_SPLIT_TONE:
			apply_split_tone(rgb, values);
			break;
		case LOOK_OP_VIGNETTE:
			scale_rgb(rgb, 1 - values[0] * vignetteDistance);
			break;
		default:
			// Raw-stage and spatial steps run on the whole frame (see look_op_runs_in_live_view).
			break;
		}
	}
	if (!isDisplay) {
		apply_tone_table(state, rgb);
	}
}

void look_render_pixel(const LookState *state, float red, float green, float blue, uint8_t out[RGB_CHANNELS])
{
	float rgb[RGB_CHANNELS] = {red, green, blue};
	render_pixel(look_get(state->look), state, rgb, 0);
	for (int i = 0; i < RGB_CHANNELS; i++) {
		out[i] = to_byte(rgb[i]);
	}
}

bool look_render_build_lut(LookLut *lut, int lookIndex)
{
	// The index is filled on first use. Its last slot is above 0 once filled.
	if (!g_lookLutIndex[LOOK_LUT_LEVEL_MAX]) {
		for (int i = 0; i < LOOK_LUT_LEVELS; i++) {
			float level = i / (float)LOOK_LUT_LEVEL_MAX;
			g_lookLutIndex[i] = (uint8_t)(sqrtf(level) * LUT_LAST_ENTRY + 0.5f);
		}
	}
	if (!lut->rgb) {
		lut->rgb = malloc(LUT_BYTES);
	}
	if (!lut->rgb) {
		return false;
	}
	LookState state;
	look_render_prepare(&state, lookIndex);
	const Look *look = look_get(lookIndex);
	// The axes are square-root spaced, so entry i holds the linear level (i / LUT_LAST_ENTRY) squared.
	float entryLevel[LOOK_LUT_SIZE];
	for (int i = 0; i < LOOK_LUT_SIZE; i++) {
		float spacing = i / (float)LUT_LAST_ENTRY;
		entryLevel[i] = spacing * spacing;
	}
	uint8_t *entry = lut->rgb;
	for (int red = 0; red < LOOK_LUT_SIZE; red++) {
		for (int green = 0; green < LOOK_LUT_SIZE; green++) {
			for (int blue = 0; blue < LOOK_LUT_SIZE; blue++) {
				float rgb[RGB_CHANNELS] = {entryLevel[red], entryLevel[green], entryLevel[blue]};
				render_pixel(look, &state, rgb, 0);
				for (int i = 0; i < RGB_CHANNELS; i++) {
					entry[i] = to_byte(rgb[i]);
				}
				entry += RGB_CHANNELS;
			}
		}
	}
	lut->look = lookIndex;
	return true;
}

// Replaces the sample at (x, y) with the median of its same-colour neighbours, read from the unchanged copy.
// Unless forced, only a sample more than threshold outside the neighbours' range is replaced. Returns true
// when the sample changed.
static bool replace_with_neighbor_median(uint16_t *bayer, const uint16_t *original, int width, int x, int y,
                                         bool force, int threshold)
{
	bool isGreen = ((x ^ y) & 1) != 0;
	int neighborCount = isGreen ? GREEN_NEIGHBOR_COUNT : RED_BLUE_NEIGHBOR_COUNT;
	int neighbors[GREEN_NEIGHBOR_COUNT];
	int lowest = ABOVE_ANY_SAMPLE;
	int highest = BELOW_ANY_SAMPLE;
	for (int k = 0; k < neighborCount; k++) {
		PixelOffset offset = SAME_COLOR_NEIGHBORS[k];
		neighbors[k] = original[(y + offset.dy) * width + x + offset.dx];
		if (neighbors[k] < lowest) {
			lowest = neighbors[k];
		}
		if (neighbors[k] > highest) {
			highest = neighbors[k];
		}
	}
	int sample = original[y * width + x];
	if (!force && sample <= highest + threshold && sample >= lowest - threshold) {
		return false;
	}
	// Insertion sort. The count is even, so the median is the mean of the two middle values.
	for (int i = 1; i < neighborCount; i++) {
		for (int j = i; j > 0 && neighbors[j - 1] > neighbors[j]; j--) {
			int swap = neighbors[j];
			neighbors[j] = neighbors[j - 1];
			neighbors[j - 1] = swap;
		}
	}
	int middle = neighborCount / 2;
	int median = (neighbors[middle - 1] + neighbors[middle]) / 2;
	if (bayer[y * width + x] == median) {
		return false;
	}
	bayer[y * width + x] = (uint16_t)median;
	return true;
}

int look_render_fix_hot_pixels(uint8_t *raw, int width, int height, int offsetX, int offsetY,
                               const uint16_t *map, int mapCount, int threshold)
{
	uint16_t *bayer = malloc((size_t)width * height * sizeof(uint16_t));
	uint16_t *original = malloc((size_t)width * height * sizeof(uint16_t));
	if (!bayer || !original) {
		free(bayer);
		free(original);
		return 0;
	}
	for (int i = 0; i < width * height; i++) {
		uint16_t sample = (uint16_t)raw10_unpack(raw + i * RAW10_BYTES_PER_PIXEL);
		original[i] = sample;
		bayer[i] = sample;
	}
	int fixedCount = 0;
	// The calibrated defects, always.
	for (int k = 0; k < mapCount; k++) {
		int x = map[k * 2] - offsetX;
		int y = map[k * 2 + 1] - offsetY;
		if (x >= EDGE_MARGIN && y >= EDGE_MARGIN && x < width - EDGE_MARGIN && y < height - EDGE_MARGIN) {
			fixedCount += replace_with_neighbor_median(bayer, original, width, x, y, true, 0);
		}
	}
	// Any other pixel that stands out.
	if (threshold > 0) {
		for (int y = EDGE_MARGIN; y < height - EDGE_MARGIN; y++) {
			for (int x = EDGE_MARGIN; x < width - EDGE_MARGIN; x++) {
				fixedCount += replace_with_neighbor_median(bayer, original, width, x, y, false, threshold);
			}
		}
	}
	for (int i = 0; i < width * height; i++) {
		raw10_pack(raw + i * RAW10_BYTES_PER_PIXEL, bayer[i]);
	}
	free(bayer);
	free(original);
	return fixedCount;
}

// The recipe's hot pixel step on 16-bit samples, in place. Neighbours are read from a copy, so one fix does
// not change the check of the next pixel.
static void fix_outliers_in_place(uint16_t *bayer, int width, int height, int threshold)
{
	size_t bytes = (size_t)width * height * sizeof(uint16_t);
	uint16_t *original = malloc(bytes);
	if (!original) {
		return;
	}
	memcpy(original, bayer, bytes);
	for (int y = EDGE_MARGIN; y < height - EDGE_MARGIN; y++) {
		for (int x = EDGE_MARGIN; x < width - EDGE_MARGIN; x++) {
			replace_with_neighbor_median(bayer, original, width, x, y, false, threshold);
		}
	}
	free(original);
}

// The hot pixel steps. Their thresholds are in 10-bit units and scale to the display white.
static void run_raw_steps(const Look *look, uint16_t *bayer, int width, int height, int displayWhite)
{
	for (int k = 0; k < look->opCount; k++) {
		const LookOp *step = &look->ops[k];
		if (step->kind == LOOK_OP_HOT_PIXEL) {
			int threshold = (int)step->values[0] * displayWhite / RAW10_WHITE;
			fix_outliers_in_place(bayer, width, height, threshold);
		}
	}
}

// HDR highlights on linear RGB where 1 is the display white and the raw's white is at range. Each channel is
// first held to range, so a highlight clipped in the raw stays white after the white balance. Above the knee
// the brightest channel m follows a shoulder f with f(knee) = knee, slope 1 there and f(range) = 1. All three
// channels scale by f(m) / m, which keeps hue and saturation.
// f(m) = knee + (1 - knee) t (1 + a) / (1 + a t), with t = (m - knee) / (range - knee) and
// a = shoulderStrength = (range - 1) / (1 - knee).
static void roll_off_highlights(float rgb[RGB_CHANNELS], float range, float shoulderStrength)
{
	float brightest = 0;
	for (int i = 0; i < RGB_CHANNELS; i++) {
		if (rgb[i] > range) {
			rgb[i] = range;
		}
		if (rgb[i] > brightest) {
			brightest = rgb[i];
		}
	}
	if (brightest <= HDR_KNEE) {
		return;
	}
	float t = (brightest - HDR_KNEE) / (range - HDR_KNEE);
	float a = shoulderStrength;
	float shoulder = HDR_KNEE + (1 - HDR_KNEE) * t * (1 + a) / (1 + a * t);
	scale_rgb(rgb, shoulder / brightest);
}

// Coordinates one step outside the frame mirror back inside, which keeps the Bayer colour.
static inline int mirror_coordinate(int coordinate, int size)
{
	if (coordinate < 0) {
		coordinate = 1;
	} else if (coordinate >= size) {
		coordinate = size - 2;
	}
	return coordinate;
}

// The sample at (x, y) above black.
static inline float sample_at(const BayerFrame *frame, int x, int y)
{
	int row = mirror_coordinate(y, frame->height);
	int column = mirror_coordinate(x, frame->width);
	return (float)frame->samples[row * frame->width + column] - frame->black;
}

static inline float cross_mean(const BayerFrame *frame, int x, int y)
{
	float above = sample_at(frame, x, y - 1);
	float below = sample_at(frame, x, y + 1);
	float left = sample_at(frame, x - 1, y);
	float right = sample_at(frame, x + 1, y);
	return (above + below + left + right) / 4;
}

static inline float diagonal_mean(const BayerFrame *frame, int x, int y)
{
	float aboveLeft = sample_at(frame, x - 1, y - 1);
	float aboveRight = sample_at(frame, x + 1, y - 1);
	float belowLeft = sample_at(frame, x - 1, y + 1);
	float belowRight = sample_at(frame, x + 1, y + 1);
	return (aboveLeft + aboveRight + belowLeft + belowRight) / 4;
}

// Bilinear RGGB demosaic: each missing colour is the mean of the nearest samples of that colour.
static void demosaic_at(const BayerFrame *frame, int x, int y, float rgb[RGB_CHANNELS])
{
	float center = sample_at(frame, x, y);
	bool isRedRow = (y & 1) == 0;
	bool isRedColumn = (x & 1) == 0;
	if (isRedRow && isRedColumn) {
		rgb[0] = center;
		rgb[1] = cross_mean(frame, x, y);
		rgb[2] = diagonal_mean(frame, x, y);
	} else if (!isRedRow && !isRedColumn) {
		rgb[2] = center;
		rgb[1] = cross_mean(frame, x, y);
		rgb[0] = diagonal_mean(frame, x, y);
	} else {
		rgb[1] = center;
		float horizontal = (sample_at(frame, x - 1, y) + sample_at(frame, x + 1, y)) / 2;
		float vertical = (sample_at(frame, x, y - 1) + sample_at(frame, x, y + 1)) / 2;
		// A green on a red row has red to its left and right.
		if (isRedRow) {
			rgb[0] = horizontal;
			rgb[2] = vertical;
		} else {
			rgb[0] = vertical;
			rgb[2] = horizontal;
		}
	}
}

// Demosaics the frame into linear, white-balanced RGB and runs the per-pixel steps into image.
static void render_pixels(const Look *look, const LookState *state, const BayerFrame *frame,
                          const float gain[RGB_CHANNELS], float range, float *image)
{
	bool isHdr = range > STANDARD_RANGE;
	float shoulderStrength = (range - 1) / (1 - HDR_KNEE);
	// Squared distances from the centre are scaled so that the corners are at 1.
	float centerX = frame->width * 0.5f;
	float centerY = frame->height * 0.5f;
	float cornerScale = 1.0f / (centerX * centerX + centerY * centerY);
	for (int y = 0; y < frame->height; y++) {
		for (int x = 0; x < frame->width; x++) {
			float rgb[RGB_CHANNELS];
			demosaic_at(frame, x, y, rgb);
			for (int i = 0; i < RGB_CHANNELS; i++) {
				rgb[i] *= gain[i];
			}
			if (isHdr) {
				roll_off_highlights(rgb, range, shoulderStrength);
			}
			float dx = x - centerX;
			float dy = y - centerY;
			render_pixel(look, state, rgb, (dx * dx + dy * dy) * cornerScale);
			float *pixel = image + ((size_t)y * frame->width + x) * RGB_CHANNELS;
			for (int i = 0; i < RGB_CHANNELS; i++) {
				pixel[i] = rgb[i];
			}
		}
	}
}

// Unsharp mask on Rec. 601 luma: each pixel moves away from the mean of its 3 x 3 box.
static void sharpen(float *image, int width, int height, float amount)
{
	float *luma = malloc((size_t)width * height * sizeof(float));
	if (!luma) {
		return;
	}
	for (int i = 0; i < width * height; i++) {
		luma[i] = luma_rec601(image + i * RGB_CHANNELS);
	}
	for (int y = SHARPEN_RADIUS; y < height - SHARPEN_RADIUS; y++) {
		for (int x = SHARPEN_RADIUS; x < width - SHARPEN_RADIUS; x++) {
			float boxSum = 0;
			for (int j = -SHARPEN_RADIUS; j <= SHARPEN_RADIUS; j++) {
				for (int i = -SHARPEN_RADIUS; i <= SHARPEN_RADIUS; i++) {
					boxSum += luma[(y + j) * width + x + i];
				}
			}
			float detail = (luma[y * width + x] - boxSum / SHARPEN_BOX_PIXELS) * amount;
			float *pixel = image + ((size_t)y * width + x) * RGB_CHANNELS;
			pixel[0] += detail;
			pixel[1] += detail;
			pixel[2] += detail;
		}
	}
	free(luma);
}

// Uniform noise in -1..1.
static float next_noise(uint32_t *seed)
{
	*seed = *seed * LCG_MULTIPLIER + LCG_INCREMENT;
	float unit = (*seed >> NOISE_SHIFT) / NOISE_SCALE;
	return (unit - 0.5f) * 2;
}

// Monochrome noise of up to amount.
static void add_grain(float *image, int width, int height, float amount, uint32_t *seed)
{
	for (int i = 0; i < width * height; i++) {
		float noise = next_noise(seed) * amount;
		float *pixel = image + i * RGB_CHANNELS;
		pixel[0] += noise;
		pixel[1] += noise;
		pixel[2] += noise;
	}
}

static void run_spatial_steps(const Look *look, float *image, int width, int height)
{
	uint32_t grainSeed = GRAIN_SEED;
	for (int k = 0; k < look->opCount; k++) {
		const LookOp *step = &look->ops[k];
		float amount = step->values[0];
		bool isActive = amount > 0;
		if (step->kind == LOOK_OP_SHARPEN && isActive) {
			sharpen(image, width, height, amount);
		} else if (step->kind == LOOK_OP_GRAIN && isActive) {
			add_grain(image, width, height, amount, &grainSeed);
		}
	}
}

// Renders 16-bit Bayer samples, which it owns: the raw-stage steps change them in place, and they are freed
// right after the demosaic so the spatial steps do not hold them too. range > 1 marks an HDR raw.
static bool render_bayer(uint16_t *bayer, int width, int height, int black, int white, float range,
                         const float whiteBalance[RGB_CHANNELS], int lookIndex, uint8_t *rgb)
{
	const Look *look = look_get(lookIndex);
	LookState state;
	look_render_prepare(&state, lookIndex);
	float *image = malloc((size_t)width * height * RGB_CHANNELS * sizeof(float));
	if (!image) {
		free(bayer);
		return false;
	}
	bool isHdr = range > STANDARD_RANGE;
	// An HDR raw's display white lies range times below its own white.
	int displayWhite = white;
	if (isHdr) {
		displayWhite = black + (int)((float)(white - black) / range + 0.5f);
	}
	run_raw_steps(look, bayer, width, height, displayWhite);
	// Linear level 1 is the display white, so an HDR raw's own white is at range.
	float whiteLevel = isHdr ? range : 1.0f;
	float scale = whiteLevel / (float)(white - black);
	float gain[RGB_CHANNELS];
	for (int i = 0; i < RGB_CHANNELS; i++) {
		gain[i] = whiteBalance[i] * scale;
	}
	BayerFrame frame = {.samples = bayer, .width = width, .height = height, .black = black};
	render_pixels(look, &state, &frame, gain, range, image);
	free(bayer);
	run_spatial_steps(look, image, width, height);
	for (int i = 0; i < width * height * RGB_CHANNELS; i++) {
		rgb[i] = to_byte(image[i]);
	}
	free(image);
	return true;
}

bool look_render(const uint8_t *raw, int width, int height, int black, int white,
                 const float whiteBalance[RGB_CHANNELS], int lookIndex, uint8_t *rgb)
{
	uint16_t *bayer = malloc((size_t)width * height * sizeof(uint16_t));
	if (!bayer) {
		return false;
	}
	for (int i = 0; i < width * height; i++) {
		bayer[i] = (uint16_t)raw10_unpack(raw + i * RAW10_BYTES_PER_PIXEL);
	}
	return render_bayer(bayer, width, height, black, white, STANDARD_RANGE, whiteBalance, lookIndex, rgb);
}

bool look_render_hdr(const uint16_t *raw, int width, int height, int black, int white, float range,
                     const float whiteBalance[RGB_CHANNELS], int lookIndex, uint8_t *rgb)
{
	size_t bytes = (size_t)width * height * sizeof(uint16_t);
	uint16_t *bayer = malloc(bytes);
	if (!bayer) {
		return false;
	}
	memcpy(bayer, raw, bytes);
	return render_bayer(bayer, width, height, black, white, range, whiteBalance, lookIndex, rgb);
}
