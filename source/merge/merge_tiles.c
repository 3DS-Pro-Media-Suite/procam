#include "merge_tiles.h"
#include <math.h>
#include <string.h>
#include "math_util.h"
#include "merge_common.h"

#define TILE_SAMPLES (MERGE_TILE_SIZE * MERGE_TILE_SIZE)
#define TILE_STRIDE_STEPS (MERGE_TILE_STRIDE * SUBPIXEL_STEPS)

// A plane's neighbouring samples are this many mosaic pixels apart.
#define BAYER_STEP 2

// One plane's weight in a mean over all planes.
#define PLANE_SHARE (1.0f / BAYER_PLANE_COUNT)

// The Catmull-Rom filter reads taps -1, 0, 1 and 2 around each sample.
#define FILTER_TAPS 4
#define FIRST_TAP (-1)
#define LAST_TAP 2
// Source samples that one filtered tile row reads.
#define FILTER_SPAN (MERGE_TILE_SIZE + FILTER_TAPS - 1)
// The integer filter weights are scaled by WEIGHT_ONE in each of the two passes.
#define WEIGHT_ONE 1024
#define FILTER_OUTPUT_SCALE (1.0f / (WEIGHT_ONE * WEIGHT_ONE))

#define BILINEAR_CORNERS 4

// The robust weights compare 3x3 windows over all planes.
#define BOX_SIZE 3
#define WINDOW_SAMPLES (BOX_SIZE * BOX_SIZE * BAYER_PLANE_COUNT)

// A robust weight falls from 1 at a test's FULL limit to 0 at its ZERO limit. The mean test scores a window's
// mean difference in standard deviations, the energy test its mean squared difference in variances.
#define MEAN_TEST_FULL 2.0f
#define MEAN_TEST_ZERO 4.0f
#define ENERGY_TEST_FULL 1.5f
#define ENERGY_TEST_ZERO 2.5f
// The merge weights use the limits as they are. A B tile's check against A looks for gross disagreement
// only, so its limits are wider by the loose factors.
#define STRICT_FACTOR 1.0f
#define LOOSE_MEAN_FACTOR 2.0f
#define LOOSE_ENERGY_FACTOR 3.0f

// Plain merge: an alt tile's DC match needs at least this many samples.
#define PLAIN_DC_MIN_SAMPLES 16
// A tile with a larger fraction of clipped reference samples stays out of the frames' usage report.
#define USAGE_MAX_CLIPPED_FRACTION 0.05

// A's share of the HDR blend falls from 1 at HDR_RAMP_START to 0 at HDR_RAMP_END of the reference's brightest
// plane. It ends before clipped samples bias what is left of A.
#define HDR_RAMP_START (0.85f * RAW10_WHITE)
#define HDR_RAMP_END (0.97f * RAW10_WHITE)
// A B tile whose check against A scores lower stays out of B's own estimate.
#define HDR_MIN_VERIFICATION 0.5f
// B contradicts a clipped A when it reads lower by more than this many noise standard deviations plus this
// fraction of A's level.
#define HDR_FLOOR_SIGMAS 3.0f
#define HDR_FLOOR_TOLERANCE 0.03f
// The median of n values has about MEDIAN_VARIANCE_FACTOR / n of one value's variance.
#define MEDIAN_VARIANCE_FACTOR 1.6f

// Catmull-Rom weights at phase t = k / SUBPIXEL_STEPS, times WEIGHT_ONE. They are exact integers.
#define CATMULL_ROM_WEIGHTS(k)                                                                        \
	{-(k) * (k) * (k) + 16 * (k) * (k) - 64 * (k), 3 * (k) * (k) * (k) - 40 * (k) * (k) + WEIGHT_ONE, \
	 -3 * (k) * (k) * (k) + 32 * (k) * (k) + 64 * (k), (k) * (k) * (k) - 8 * (k) * (k)}

static const int16_t g_catmullRomWeights[SUBPIXEL_STEPS][FILTER_TAPS] = {
        CATMULL_ROM_WEIGHTS(0), CATMULL_ROM_WEIGHTS(1), CATMULL_ROM_WEIGHTS(2), CATMULL_ROM_WEIGHTS(3),
        CATMULL_ROM_WEIGHTS(4), CATMULL_ROM_WEIGHTS(5), CATMULL_ROM_WEIGHTS(6), CATMULL_ROM_WEIGHTS(7)};

// One plane of a mosaic, the tile (tileY, tileX) displaced by (offsetY, offsetX) subpixel steps, into out. A
// separable Catmull-Rom filter, with the frame's edges clamped.
static void gather_tile(const uint16_t *mosaic, int plane, int tileY, int tileX, int offsetY, int offsetX,
                        float *out)
{
	int wholeY = merge_floor_div(offsetY, SUBPIXEL_STEPS);
	int wholeX = merge_floor_div(offsetX, SUBPIXEL_STEPS);
	int phaseY = offsetY - wholeY * SUBPIXEL_STEPS;
	int phaseX = offsetX - wholeX * SUBPIXEL_STEPS;
	int firstRow = tileY * MERGE_TILE_STRIDE + wholeY;
	int firstColumn = tileX * MERGE_TILE_STRIDE + wholeX;
	// Planes are numbered row by row inside the 2x2 Bayer quad. Sample (y, x) of the plane is at
	// start[BAYER_STEP * (y * MERGE_WIDTH + x)].
	const uint16_t *start = mosaic + (plane >> 1) * MERGE_WIDTH + (plane & 1);
	// Whole-pixel offsets: the filter would return the samples unchanged.
	if (!phaseY && !phaseX) {
		bool columnsInside = firstColumn >= 0 && firstColumn + MERGE_TILE_SIZE <= PLANE_WIDTH;
		for (int y = 0; y < MERGE_TILE_SIZE; y++) {
			int sourceRow = math_clamp_int(firstRow + y, 0, PLANE_HEIGHT - 1);
			const uint16_t *row = start + BAYER_STEP * sourceRow * MERGE_WIDTH;
			if (columnsInside) {
				for (int x = 0; x < MERGE_TILE_SIZE; x++) {
					out[y * MERGE_TILE_SIZE + x] = row[BAYER_STEP * (firstColumn + x)];
				}
			} else {
				for (int x = 0; x < MERGE_TILE_SIZE; x++) {
					int sourceColumn = math_clamp_int(firstColumn + x, 0, PLANE_WIDTH - 1);
					out[y * MERGE_TILE_SIZE + x] = row[BAYER_STEP * sourceColumn];
				}
			}
		}
		return;
	}
	// Integer arithmetic rounds once and is faster than float on the 3DS. Samples and weights fit 16 bits, so
	// the vertical pass uses 16-bit multiplies, and the sums fit 32 bits.
	const int16_t weightY0 = g_catmullRomWeights[phaseY][0];
	const int16_t weightY1 = g_catmullRomWeights[phaseY][1];
	const int16_t weightY2 = g_catmullRomWeights[phaseY][2];
	const int16_t weightY3 = g_catmullRomWeights[phaseY][3];
	const int16_t weightX0 = g_catmullRomWeights[phaseX][0];
	const int16_t weightX1 = g_catmullRomWeights[phaseX][1];
	const int16_t weightX2 = g_catmullRomWeights[phaseX][2];
	const int16_t weightX3 = g_catmullRomWeights[phaseX][3];
	int32_t vertical[MERGE_TILE_SIZE][FILTER_SPAN];
	int sourceColumns[FILTER_SPAN];
	const uint16_t *sourceRows[FILTER_SPAN];
	for (int k = 0; k < FILTER_SPAN; k++) {
		int sourceColumn = math_clamp_int(firstColumn + FIRST_TAP + k, 0, PLANE_WIDTH - 1);
		int sourceRow = math_clamp_int(firstRow + FIRST_TAP + k, 0, PLANE_HEIGHT - 1);
		sourceColumns[k] = BAYER_STEP * sourceColumn;
		sourceRows[k] = start + BAYER_STEP * sourceRow * MERGE_WIDTH;
	}
	for (int y = 0; y < MERGE_TILE_SIZE; y++) {
		const uint16_t *row0 = sourceRows[y];
		const uint16_t *row1 = sourceRows[y + 1];
		const uint16_t *row2 = sourceRows[y + 2];
		const uint16_t *row3 = sourceRows[y + 3];
		for (int x = 0; x < FILTER_SPAN; x++) {
			int column = sourceColumns[x];
			vertical[y][x] = weightY0 * (int16_t)row0[column] + weightY1 * (int16_t)row1[column] +
			                 weightY2 * (int16_t)row2[column] + weightY3 * (int16_t)row3[column];
		}
	}
	for (int y = 0; y < MERGE_TILE_SIZE; y++) {
		const int32_t *line = vertical[y];
		for (int x = 0; x < MERGE_TILE_SIZE; x++) {
			int32_t sum = weightX0 * line[x] + weightX1 * line[x + 1] + weightX2 * line[x + 2] +
			              weightX3 * line[x + 3];
			out[y * MERGE_TILE_SIZE + x] = (float)sum * FILTER_OUTPUT_SCALE;
		}
	}
}

// HDR: flags the samples of a gathered tile (gather_tile's arguments) that have a saturated raw sample
// anywhere in their filter support. Their value mixes in one whose true level is unknown, so they count as
// clipped.
static void gather_clip_flags(const uint16_t *mosaic, int plane, int tileY, int tileX, int offsetY,
                              int offsetX, uint8_t *clipped)
{
	int wholeY = merge_floor_div(offsetY, SUBPIXEL_STEPS);
	int wholeX = merge_floor_div(offsetX, SUBPIXEL_STEPS);
	int phaseY = offsetY - wholeY * SUBPIXEL_STEPS;
	int phaseX = offsetX - wholeX * SUBPIXEL_STEPS;
	int firstRow = tileY * MERGE_TILE_STRIDE + wholeY;
	int firstColumn = tileX * MERGE_TILE_STRIDE + wholeX;
	// Only taps with a nonzero weight count. At a whole-pixel phase that is the sample itself.
	int firstTapY = phaseY ? FIRST_TAP : 0;
	int lastTapY = phaseY ? LAST_TAP : 0;
	int firstTapX = phaseX ? FIRST_TAP : 0;
	int lastTapX = phaseX ? LAST_TAP : 0;
	const uint16_t *start = mosaic + (plane >> 1) * MERGE_WIDTH + (plane & 1);
	// Per source row: saturation ORed over each output column's taps.
	uint8_t rowFlags[FILTER_SPAN][MERGE_TILE_SIZE];
	uint8_t saturated[FILTER_SPAN];
	for (int k = 0; k < FILTER_SPAN; k++) {
		int sourceRow = math_clamp_int(firstRow + FIRST_TAP + k, 0, PLANE_HEIGHT - 1);
		const uint16_t *row = start + BAYER_STEP * sourceRow * MERGE_WIDTH;
		for (int x = 0; x < FILTER_SPAN; x++) {
			int sourceColumn = math_clamp_int(firstColumn + FIRST_TAP + x, 0, PLANE_WIDTH - 1);
			saturated[x] = row[BAYER_STEP * sourceColumn] >= INPUT_CLIP;
		}
		for (int x = 0; x < MERGE_TILE_SIZE; x++) {
			uint8_t any = 0;
			for (int tap = firstTapX; tap <= lastTapX; tap++) {
				any |= saturated[x - FIRST_TAP + tap];
			}
			rowFlags[k][x] = any;
		}
	}
	for (int y = 0; y < MERGE_TILE_SIZE; y++) {
		for (int x = 0; x < MERGE_TILE_SIZE; x++) {
			uint8_t any = 0;
			for (int tap = firstTapY; tap <= lastTapY; tap++) {
				any |= rowFlags[y - FIRST_TAP + tap][x];
			}
			clipped[y * MERGE_TILE_SIZE + x] = any;
		}
	}
}

// HDR: clamps a gathered tile (gather_tile's arguments) to the range of each sample's nearest 2x2, its
// bilinear support. Catmull-Rom overshoots about 7 % of an edge, and camera B's edges are scaled up by the
// exposure ratio.
static void dering_tile(const uint16_t *mosaic, int plane, int tileY, int tileX, int offsetY, int offsetX,
                        float *out)
{
	int wholeY = merge_floor_div(offsetY, SUBPIXEL_STEPS);
	int wholeX = merge_floor_div(offsetX, SUBPIXEL_STEPS);
	int phaseY = offsetY - wholeY * SUBPIXEL_STEPS;
	int phaseX = offsetX - wholeX * SUBPIXEL_STEPS;
	if (!phaseY && !phaseX) {
		return;
	}
	int firstRow = tileY * MERGE_TILE_STRIDE + wholeY;
	int firstColumn = tileX * MERGE_TILE_STRIDE + wholeX;
	const uint16_t *start = mosaic + (plane >> 1) * MERGE_WIDTH + (plane & 1);
	int nextRow = phaseY > 0;
	int nextColumn = phaseX > 0;
	for (int y = 0; y < MERGE_TILE_SIZE; y++) {
		int upperRow = math_clamp_int(firstRow + y, 0, PLANE_HEIGHT - 1);
		int lowerRow = math_clamp_int(firstRow + y + nextRow, 0, PLANE_HEIGHT - 1);
		const uint16_t *upper = start + BAYER_STEP * upperRow * MERGE_WIDTH;
		const uint16_t *lower = start + BAYER_STEP * lowerRow * MERGE_WIDTH;
		for (int x = 0; x < MERGE_TILE_SIZE; x++) {
			int left = BAYER_STEP * math_clamp_int(firstColumn + x, 0, PLANE_WIDTH - 1);
			int right = BAYER_STEP * math_clamp_int(firstColumn + x + nextColumn, 0, PLANE_WIDTH - 1);
			int corners[BILINEAR_CORNERS] = {upper[left], upper[right], lower[left], lower[right]};
			int low = corners[0];
			int high = corners[0];
			for (int k = 1; k < BILINEAR_CORNERS; k++) {
				if (corners[k] < low) {
					low = corners[k];
				}
				if (corners[k] > high) {
					high = corners[k];
				}
			}
			float *sample = &out[y * MERGE_TILE_SIZE + x];
			*sample = math_clamp_float(*sample, (float)low, (float)high);
		}
	}
}

// 3x3 mean with the edges replicated. The summation order is part of the result.
static void box_mean_3x3(const float *in, float *out)
{
	float rowSums[TILE_SAMPLES];
	for (int y = 0; y < MERGE_TILE_SIZE; y++) {
		const float *row = in + y * MERGE_TILE_SIZE;
		for (int x = 0; x < MERGE_TILE_SIZE; x++) {
			float left = row[x > 0 ? x - 1 : 0];
			float centre = row[x];
			float right = row[x < MERGE_TILE_SIZE - 1 ? x + 1 : MERGE_TILE_SIZE - 1];
			rowSums[y * MERGE_TILE_SIZE + x] = (left + centre) + right;
		}
	}
	const float inverseArea = 1.0f / (BOX_SIZE * BOX_SIZE);
	for (int y = 0; y < MERGE_TILE_SIZE; y++) {
		const float *upper = rowSums + (y > 0 ? y - 1 : 0) * MERGE_TILE_SIZE;
		const float *middle = rowSums + y * MERGE_TILE_SIZE;
		const float *lower =
		        rowSums + (y < MERGE_TILE_SIZE - 1 ? y + 1 : MERGE_TILE_SIZE - 1) * MERGE_TILE_SIZE;
		for (int x = 0; x < MERGE_TILE_SIZE; x++) {
			out[y * MERGE_TILE_SIZE + x] = ((upper[x] + middle[x]) + lower[x]) * inverseArea;
		}
	}
}

static void smooth_in_place(float *plane, float *temporary)
{
	box_mean_3x3(plane, temporary);
	memcpy(plane, temporary, sizeof(float) * TILE_SAMPLES);
}

// The planes' differences at one sample: their mean into scratch->meanDifference, their mean square into
// scratch->meanSquaredDifference.
static inline void store_difference_moments(TileScratch *scratch, int i, const float *difference)
{
	float sum = ((difference[0] + difference[1]) + difference[2]) + difference[3];
	float squares = ((difference[0] * difference[0] + difference[1] * difference[1]) +
	                 difference[2] * difference[2]) +
	                difference[3] * difference[3];
	scratch->meanDifference[i] = PLANE_SHARE * sum;
	scratch->meanSquaredDifference[i] = PLANE_SHARE * squares;
}

// One sample's robust weight from its mean test and energy test scores. The factors widen the limits.
static inline float robust_weight(float meanScore, float energyScore, float meanFactor, float energyFactor)
{
	const float meanSlope = 1.0f / (MEAN_TEST_ZERO - MEAN_TEST_FULL);
	const float energySlope = 1.0f / (ENERGY_TEST_ZERO - ENERGY_TEST_FULL);
	float meanWeight = (meanFactor * MEAN_TEST_ZERO - meanScore) * (meanSlope / meanFactor);
	float energyWeight = (energyFactor * ENERGY_TEST_ZERO - energyScore) * (energySlope / energyFactor);
	meanWeight = math_clamp_float(meanWeight, 0.0f, 1.0f);
	energyWeight = math_clamp_float(energyWeight, 0.0f, 1.0f);
	return energyWeight < meanWeight ? energyWeight : meanWeight;
}

// Gathers the reference tile into scratch->referenceSamples. Returns how many of its samples are clipped.
static int gather_reference(const MergeContext *context, TileScratch *scratch, int tileY, int tileX)
{
	const uint16_t *reference = context->framesA[context->referenceIndex];
	int clippedCount = 0;
	for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
		gather_tile(reference, plane, tileY, tileX, 0, 0, scratch->referenceSamples[plane]);
		for (int i = 0; i < TILE_SAMPLES; i++) {
			clippedCount += scratch->referenceSamples[plane][i] >= (float)INPUT_CLIP;
		}
	}
	return clippedCount;
}

static bool counts_for_usage(int clippedCount)
{
	return clippedCount <= USAGE_MAX_CLIPPED_FRACTION * BAYER_PLANE_COUNT * TILE_SAMPLES;
}

// Adds an alt tile's mean weight to its frame's usage in this tile row.
static void record_usage(MergeContext *context, int tileY, int altIndex, double weightSum, int sampleCount)
{
	context->rowUse[tileY][altIndex] += weightSum / sampleCount;
	context->rowUseCount[tileY][altIndex]++;
}

// Whether an alt tile displaced by (offsetY, offsetX) subpixel steps lies wholly inside the frame.
static bool alt_tile_inside(int tileY, int tileX, int offsetY, int offsetX)
{
	int top = tileY * TILE_STRIDE_STEPS + offsetY;
	int left = tileX * TILE_STRIDE_STEPS + offsetX;
	int size = MERGE_TILE_SIZE * SUBPIXEL_STEPS;
	return top >= 0 && left >= 0 && top + size <= PLANE_HEIGHT * SUBPIXEL_STEPS &&
	       left + size <= PLANE_WIDTH * SUBPIXEL_STEPS;
}

// Adds one alt sample, weighted, to the sums of the planes where it is not clipped.
static inline void add_alt_sample(TileScratch *scratch, int i, float weight)
{
	for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
		if (!scratch->altClipped[plane][i]) {
			scratch->weightedSum[plane][i] += weight * scratch->altSamples[plane][i];
			scratch->weightSum[plane][i] += weight;
		}
	}
}

// Shifts an alt tile by its mean difference to the reference over the samples in span that are unclipped in
// the alt and below referenceLimit in the reference. It needs at least minSamples of them.
static void match_tile_dc(const float *reference, float *alt, const uint8_t *clipped, const SampleSpan *span,
                          float referenceLimit, int minSamples)
{
	float referenceSum = 0;
	float altSum = 0;
	int count = 0;
	for (int y = span->firstRow; y < span->endRow; y++) {
		for (int x = span->firstColumn; x < span->endColumn; x++) {
			int i = y * MERGE_TILE_SIZE + x;
			if (!clipped[i] && reference[i] < referenceLimit) {
				referenceSum += reference[i];
				altSum += alt[i];
				count++;
			}
		}
	}
	if (count < minSamples) {
		return;
	}
	float shift = (referenceSum - altSum) / (float)count;
	for (int i = 0; i < TILE_SAMPLES; i++) {
		alt[i] += shift;
	}
}

// Plain merge: the reference's noise variance (3x3x4 means) into scratch->referenceVariance. The reference
// starts the sums.
static void plain_reference_noise(const MergeContext *context, TileScratch *scratch)
{
	const float *blackA = context->job->blackA;
	float shotNoise = context->shotNoiseScaleA;
	float readNoise = context->readNoiseVarianceA;
	for (int i = 0; i < TILE_SAMPLES; i++) {
		scratch->referenceVariance[i] = 0;
	}
	for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
		box_mean_3x3(scratch->referenceSamples[plane], scratch->scratchPlane);
		for (int i = 0; i < TILE_SAMPLES; i++) {
			float variance =
			        merge_noise_variance(scratch->scratchPlane[i], blackA[plane], shotNoise, readNoise);
			scratch->referenceVariance[i] += PLANE_SHARE * variance;
		}
		memcpy(scratch->weightedSum[plane], scratch->referenceSamples[plane],
		       sizeof scratch->weightedSum[plane]);
		for (int i = 0; i < TILE_SAMPLES; i++) {
			scratch->weightSum[plane][i] = 1.0f;
		}
	}
	smooth_in_place(scratch->referenceVariance, scratch->scratchPlane);
}

// Plain merge: gathers an alt tile into scratch->altSamples with its clipped samples in scratch->altClipped,
// mapped to A's levels and DC matched when the frame asks for it. Returns its noise variance, a mean over the
// planes.
static float gather_plain_alt(const MergeContext *context, TileScratch *scratch, const AltFrame *alt,
                              int tileY, int tileX, int offsetY, int offsetX)
{
	const float *blackA = context->job->blackA;
	const float *blackB = context->job->blackB;
	const float *offset = context->levelOffset;
	float shotNoise = context->shotNoiseScaleA;
	float readNoise = context->readNoiseVarianceA;
	const SampleSpan wholeTile = {
	        .firstRow = 0, .endRow = MERGE_TILE_SIZE, .firstColumn = 0, .endColumn = MERGE_TILE_SIZE};
	float variance = 0;
	for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
		float *altPlane = scratch->altSamples[plane];
		uint8_t *clipped = scratch->altClipped[plane];
		gather_tile(alt->mosaic, plane, tileY, tileX, offsetY, offsetX, altPlane);
		float sum = 0;
		for (int i = 0; i < TILE_SAMPLES; i++) {
			sum += altPlane[i];
			clipped[i] = altPlane[i] >= (float)INPUT_CLIP;
		}
		float rawMean = sum / (float)TILE_SAMPLES;
		if (alt->levelsMapped) {
			float gain = context->levelGain[plane];
			float rawVariance = merge_noise_variance(rawMean, blackB[plane], shotNoise, readNoise);
			variance += PLANE_SHARE * rawVariance * gain * gain;
			// A sample clipped in camera B is at least A's white.
			for (int i = 0; i < TILE_SAMPLES; i++) {
				float level = gain * (altPlane[i] - blackB[plane]) + offset[plane] + blackA[plane];
				if (clipped[i] && level < (float)RAW10_WHITE) {
					level = (float)RAW10_WHITE;
				}
				altPlane[i] = level;
			}
		} else {
			variance += PLANE_SHARE * merge_noise_variance(rawMean, blackA[plane], shotNoise, readNoise);
		}
		if (alt->dcMatched) {
			match_tile_dc(scratch->referenceSamples[plane], altPlane, clipped, &wholeTile, (float)INPUT_CLIP,
			              PLAIN_DC_MIN_SAMPLES);
		}
	}
	return variance;
}

// Plain merge: weighs an alt tile against the reference and adds it to the sums. Returns its weight sum.
static double merge_plain_alt(TileScratch *scratch, float altVariance)
{
	for (int i = 0; i < TILE_SAMPLES; i++) {
		float difference[BAYER_PLANE_COUNT];
		for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
			difference[plane] = scratch->referenceSamples[plane][i] - scratch->altSamples[plane][i];
		}
		store_difference_moments(scratch, i, difference);
	}
	box_mean_3x3(scratch->meanDifference, scratch->scratchPlane);
	box_mean_3x3(scratch->meanSquaredDifference, scratch->smoothedSquaredDifference);
	double weightSum = 0;
	for (int i = 0; i < TILE_SAMPLES; i++) {
		float inverseVariance = 1.0f / (scratch->referenceVariance[i] + altVariance);
		float meanScore = fabsf(scratch->scratchPlane[i]) * sqrtf(WINDOW_SAMPLES * inverseVariance);
		float energyScore = scratch->smoothedSquaredDifference[i] * inverseVariance;
		float weight = robust_weight(meanScore, energyScore, STRICT_FACTOR, STRICT_FACTOR);
		weightSum += weight;
		add_alt_sample(scratch, i, weight);
	}
	return weightSum;
}

void merge_tiles_plain(MergeContext *context, TileScratch *scratch, int tileY, int tileX)
{
	int clippedCount = gather_reference(context, scratch, tileY, tileX);
	bool countsForUsage = counts_for_usage(clippedCount);
	plain_reference_noise(context, scratch);
	for (int altIndex = 0; altIndex < context->altCount; altIndex++) {
		const AltFrame *alt = &context->alts[altIndex];
		int offsetY = alt->offsets[tileY][tileX][AXIS_Y];
		int offsetX = alt->offsets[tileY][tileX][AXIS_X];
		// A tile reaching outside the frame gets no weight.
		if (!alt_tile_inside(tileY, tileX, offsetY, offsetX)) {
			continue;
		}
		float altVariance = gather_plain_alt(context, scratch, alt, tileY, tileX, offsetY, offsetX);
		double weightSum = merge_plain_alt(scratch, altVariance);
		if (countsForUsage) {
			record_usage(context, tileY, altIndex, weightSum, TILE_SAMPLES);
		}
	}
}

// HDR: camera B's noise variance in A's units at a level in A's units, black included. It is B's own model at
// the raw level that maps there, times the gain squared.
static inline float noise_variance_b(const MergeContext *context, int plane, float level)
{
	float gain = context->levelGain[plane];
	float signal = level - context->levelOffset[plane] - context->job->blackA[plane];
	return gain * context->shotNoiseScaleB * (signal > 0 ? signal : 0.0f) +
	       gain * gain * context->readNoiseVarianceB;
}

// HDR: the samples [*first, *end) of a tile, along one axis, whose bilinear support lies inside the frame.
// start is the tile's first sample in subpixel steps, frameSize the frame's size in plane pixels. Camera B is
// the only source for A's clipped samples, so the part of a B tile inside its frame still counts.
static void span_inside_frame(int start, int frameSize, int *first, int *end)
{
	int whole = merge_floor_div(start, SUBPIXEL_STEPS);
	int phase = start - whole * SUBPIXEL_STEPS;
	// Sample k reads whole + k, and also the next sample when there is a phase.
	int reach = phase ? 1 : 0;
	*first = math_clamp_int(-whole, 0, MERGE_TILE_SIZE);
	*end = math_clamp_int(frameSize - reach - whole, 0, MERGE_TILE_SIZE);
}

// HDR: the robust weights of an alt tile against a reference tile (all planes, A's units) into weights. One
// sample's difference has the variance variance[i] * varianceScale.
// capAtClip: where the reference is at A's clip, the alt counts as at most the reference, because B above
// A's clip agrees with A there. Samples flagged in clippedFirst or clippedSecond (either may be NULL) count
// as agreeing: they are no evidence either way. looseWeights (may be NULL) gets the weights with the loose
// limits.
static void hdr_robust_weights(TileScratch *scratch, float (*reference)[TILE_SAMPLES],
                               float (*alt)[TILE_SAMPLES], bool capAtClip,
                               uint8_t (*clippedFirst)[TILE_SAMPLES], uint8_t (*clippedSecond)[TILE_SAMPLES],
                               const float *variance, float varianceScale, float *weights,
                               float *looseWeights)
{
	for (int i = 0; i < TILE_SAMPLES; i++) {
		float difference[BAYER_PLANE_COUNT];
		for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
			float referenceValue = reference[plane][i];
			float altValue = alt[plane][i];
			if (capAtClip && referenceValue >= (float)INPUT_CLIP && altValue > referenceValue) {
				altValue = referenceValue;
			}
			bool noEvidence =
			        (clippedFirst && clippedFirst[plane][i]) || (clippedSecond && clippedSecond[plane][i]);
			difference[plane] = noEvidence ? 0.0f : referenceValue - altValue;
		}
		store_difference_moments(scratch, i, difference);
	}
	box_mean_3x3(scratch->meanDifference, scratch->scratchPlane);
	box_mean_3x3(scratch->meanSquaredDifference, scratch->smoothedSquaredDifference);
	for (int i = 0; i < TILE_SAMPLES; i++) {
		float inverseVariance = 1.0f / (variance[i] * varianceScale);
		float meanScore = fabsf(scratch->scratchPlane[i]) * sqrtf(WINDOW_SAMPLES * inverseVariance);
		float energyScore = scratch->smoothedSquaredDifference[i] * inverseVariance;
		weights[i] = robust_weight(meanScore, energyScore, STRICT_FACTOR, STRICT_FACTOR);
		if (looseWeights) {
			looseWeights[i] = robust_weight(meanScore, energyScore, LOOSE_MEAN_FACTOR, LOOSE_ENERGY_FACTOR);
		}
	}
}

// HDR: the reference's 3x3 means (hdr->referenceLocalMean), A's noise there (scratch->referenceVariance), B's
// mapped noise there (hdr->bVarianceAtReference) and B's weight in A's estimate (hdr->bWeightInA): A's
// variance over B's, per plane. The reference starts the sums.
static void hdr_reference_noise(const MergeContext *context, TileScratch *scratch, HdrScratch *hdr)
{
	const float *blackA = context->job->blackA;
	float shotNoise = context->shotNoiseScaleA;
	float readNoise = context->readNoiseVarianceA;
	for (int i = 0; i < TILE_SAMPLES; i++) {
		hdr->bVarianceAtReference[i] = 0;
		scratch->referenceVariance[i] = 0;
	}
	for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
		box_mean_3x3(scratch->referenceSamples[plane], hdr->referenceLocalMean[plane]);
		for (int i = 0; i < TILE_SAMPLES; i++) {
			float mean = hdr->referenceLocalMean[plane][i];
			float varianceA = merge_noise_variance(mean, blackA[plane], shotNoise, readNoise);
			float varianceB = noise_variance_b(context, plane, mean);
			scratch->referenceVariance[i] += PLANE_SHARE * varianceA;
			hdr->bVarianceAtReference[i] += PLANE_SHARE * varianceB;
			hdr->bWeightInA[plane][i] = varianceA / varianceB;
			scratch->weightedSum[plane][i] = scratch->referenceSamples[plane][i];
			scratch->weightSum[plane][i] = 1.0f;
		}
	}
	smooth_in_place(scratch->referenceVariance, scratch->scratchPlane);
	smooth_in_place(hdr->bVarianceAtReference, scratch->scratchPlane);
}

// HDR: A's share of the blend per sample (hdr->aShare), from the brightest plane's 3x3 mean. Returns whether
// it is below 1 anywhere.
static bool hdr_a_share(HdrScratch *hdr)
{
	const float inverseRamp = 1.0f / (HDR_RAMP_END - HDR_RAMP_START);
	bool belowOne = false;
	for (int i = 0; i < TILE_SAMPLES; i++) {
		float brightest = hdr->referenceLocalMean[0][i];
		for (int plane = 1; plane < BAYER_PLANE_COUNT; plane++) {
			if (hdr->referenceLocalMean[plane][i] > brightest) {
				brightest = hdr->referenceLocalMean[plane][i];
			}
		}
		float share = math_clamp_float((HDR_RAMP_END - brightest) * inverseRamp, 0.0f, 1.0f);
		hdr->aShare[i] = share;
		if (share < 1) {
			belowOne = true;
		}
	}
	return belowOne;
}

// HDR: camera A's alt frames, weighted as in the plain merge.
static void hdr_merge_alts_a(MergeContext *context, TileScratch *scratch, HdrScratch *hdr, int tileY,
                             int tileX, bool countsForUsage)
{
	const float *blackA = context->job->blackA;
	float shotNoise = context->shotNoiseScaleA;
	float readNoise = context->readNoiseVarianceA;
	for (int altIndex = 0; altIndex < context->altCountA; altIndex++) {
		const AltFrame *alt = &context->alts[altIndex];
		int offsetY = alt->offsets[tileY][tileX][AXIS_Y];
		int offsetX = alt->offsets[tileY][tileX][AXIS_X];
		if (!alt_tile_inside(tileY, tileX, offsetY, offsetX)) {
			continue;
		}
		float altVariance = 0;
		for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
			gather_tile(alt->mosaic, plane, tileY, tileX, offsetY, offsetX, scratch->altSamples[plane]);
			gather_clip_flags(alt->mosaic, plane, tileY, tileX, offsetY, offsetX, scratch->altClipped[plane]);
			float sum = 0;
			for (int i = 0; i < TILE_SAMPLES; i++) {
				sum += scratch->altSamples[plane][i];
			}
			float mean = sum / (float)TILE_SAMPLES;
			altVariance += PLANE_SHARE * merge_noise_variance(mean, blackA[plane], shotNoise, readNoise);
		}
		for (int i = 0; i < TILE_SAMPLES; i++) {
			hdr->sampleVariance[i] = scratch->referenceVariance[i] + altVariance;
		}
		hdr_robust_weights(scratch, scratch->referenceSamples, scratch->altSamples, false, NULL, NULL,
		                   hdr->sampleVariance, 1.0f, hdr->weights, NULL);
		double weightSum = 0;
		for (int i = 0; i < TILE_SAMPLES; i++) {
			float weight = hdr->weights[i];
			weightSum += weight;
			add_alt_sample(scratch, i, weight);
		}
		if (countsForUsage) {
			record_usage(context, tileY, altIndex, weightSum, TILE_SAMPLES);
		}
	}
}

// HDR: gathers a camera B tile into scratch->altSamples, deringed, mapped to A's levels and DC matched over
// span, with its clipped samples in scratch->altClipped.
static void gather_b_tile(const MergeContext *context, TileScratch *scratch, const AltFrame *alt, int tileY,
                          int tileX, int offsetY, int offsetX, const SampleSpan *span)
{
	const float *blackA = context->job->blackA;
	const float *blackB = context->job->blackB;
	const float *gain = context->levelGain;
	const float *offset = context->levelOffset;
	for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
		float *altPlane = scratch->altSamples[plane];
		uint8_t *clipped = scratch->altClipped[plane];
		gather_tile(alt->mosaic, plane, tileY, tileX, offsetY, offsetX, altPlane);
		dering_tile(alt->mosaic, plane, tileY, tileX, offsetY, offsetX, altPlane);
		gather_clip_flags(alt->mosaic, plane, tileY, tileX, offsetY, offsetX, clipped);
		// B covers the exposure ratio times A's range, so no clipped sample is raised.
		for (int i = 0; i < TILE_SAMPLES; i++) {
			altPlane[i] = gain[plane] * (altPlane[i] - blackB[plane]) + offset[plane] + blackA[plane];
		}
		// Nearer A's clip, the samples A has left are those whose noise came out low, so they stay out.
		match_tile_dc(scratch->referenceSamples[plane], altPlane, clipped, span, HDR_RAMP_START,
		              HDR_DC_MIN_SAMPLES);
	}
}

static bool reference_unclipped(const TileScratch *scratch, int i)
{
	for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
		if (scratch->referenceSamples[plane][i] >= (float)INPUT_CLIP) {
			return false;
		}
	}
	return true;
}

// HDR: adds a B tile to A's estimate over span, weighted by B's noise relative to A's, where neither is
// clipped. Returns the weight sum. *verification gets the mean loose weight over the positions where A is
// unclipped: a B tile in the wrong place, or seeing what A does not, shows there.
static double merge_b_into_a(TileScratch *scratch, const HdrScratch *hdr, const SampleSpan *span,
                             float *verification)
{
	double weightSum = 0;
	double looseSum = 0;
	int unclippedCount = 0;
	for (int y = span->firstRow; y < span->endRow; y++) {
		for (int x = span->firstColumn; x < span->endColumn; x++) {
			int i = y * MERGE_TILE_SIZE + x;
			float weight = hdr->weights[i];
			weightSum += weight;
			if (reference_unclipped(scratch, i)) {
				looseSum += hdr->looseWeights[i];
				unclippedCount++;
			}
			for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
				if (!scratch->altClipped[plane][i] &&
				    scratch->referenceSamples[plane][i] < (float)INPUT_CLIP) {
					float weightB = weight * hdr->bWeightInA[plane][i];
					scratch->weightedSum[plane][i] += weightB * scratch->altSamples[plane][i];
					scratch->weightSum[plane][i] += weightB;
				}
			}
		}
	}
	if (unclippedCount >= HDR_CHECK_MIN_POSITIONS) {
		*verification = (float)(looseSum / unclippedCount);
	} else {
		*verification = 1.0f;
	}
	return weightSum;
}

static void keep_b_tile(const TileScratch *scratch, HdrScratch *hdr, int kept, const SampleSpan *span,
                        float verification)
{
	memcpy(hdr->keptBSamples[kept], scratch->altSamples, sizeof hdr->keptBSamples[kept]);
	memcpy(hdr->keptBClipped[kept], scratch->altClipped, sizeof hdr->keptBClipped[kept]);
	hdr->span[kept] = *span;
	hdr->keptBVerification[kept] = verification;
}

// HDR: camera B's frames add to A's estimate. Those that pass their check against A are kept for B's own
// estimate when it is needed. Returns the number kept.
static int hdr_merge_alts_b(MergeContext *context, TileScratch *scratch, HdrScratch *hdr, int tileY,
                            int tileX, bool countsForUsage, bool needB)
{
	int keptCount = 0;
	for (int altIndex = context->altCountA; altIndex < context->altCount; altIndex++) {
		const AltFrame *alt = &context->alts[altIndex];
		int offsetY = alt->offsets[tileY][tileX][AXIS_Y];
		int offsetX = alt->offsets[tileY][tileX][AXIS_X];
		SampleSpan span;
		span_inside_frame(tileY * TILE_STRIDE_STEPS + offsetY, PLANE_HEIGHT, &span.firstRow, &span.endRow);
		span_inside_frame(tileX * TILE_STRIDE_STEPS + offsetX, PLANE_WIDTH, &span.firstColumn,
		                  &span.endColumn);
		if (span.firstRow >= span.endRow || span.firstColumn >= span.endColumn) {
			continue;
		}
		gather_b_tile(context, scratch, alt, tileY, tileX, offsetY, offsetX, &span);
		for (int i = 0; i < TILE_SAMPLES; i++) {
			hdr->sampleVariance[i] = scratch->referenceVariance[i] + hdr->bVarianceAtReference[i];
		}
		hdr_robust_weights(scratch, scratch->referenceSamples, scratch->altSamples, true, NULL, NULL,
		                   hdr->sampleVariance, 1.0f, hdr->weights, hdr->looseWeights);
		float verification;
		double weightSum = merge_b_into_a(scratch, hdr, &span, &verification);
		if (countsForUsage) {
			int sampleCount = (span.endRow - span.firstRow) * (span.endColumn - span.firstColumn);
			record_usage(context, tileY, altIndex, weightSum, sampleCount);
		}
		if (!needB || verification < HDR_MIN_VERIFICATION) {
			continue;
		}
		keep_b_tile(scratch, hdr, keptCount, &span, verification);
		keptCount++;
	}
	return keptCount;
}

// HDR: clears B's own estimate. The reference stands in for the median where there is none, so the 3x3 means
// have a value.
static void hdr_prepare_b_estimate(const TileScratch *scratch, HdrScratch *hdr, bool needB)
{
	// No evidence against B where it has nothing.
	for (int i = 0; i < TILE_SAMPLES; i++) {
		hdr->agree[i] = 1.0f;
	}
	if (!needB) {
		return;
	}
	for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
		for (int i = 0; i < TILE_SAMPLES; i++) {
			hdr->weightSumB[plane][i] = 0;
			hdr->weightedSumB[plane][i] = 0;
			hdr->medianMissing[plane][i] = 1;
			hdr->bMedian[plane][i] = scratch->referenceSamples[plane][i];
		}
	}
}

// Insertion sort step: there are at most MERGE_MAX_FRAMES values.
static inline void insert_sorted(float *values, int count, float value)
{
	int position = count;
	while (position > 0 && values[position - 1] > value) {
		values[position] = values[position - 1];
		position--;
	}
	values[position] = value;
}

static inline float median_of_sorted(const float *values, int count)
{
	int middle = count / 2;
	if (count & 1) {
		return values[middle];
	}
	return 0.5f * (values[middle - 1] + values[middle]);
}

static bool span_contains(const SampleSpan *span, int y, int x)
{
	return y >= span->firstRow && y < span->endRow && x >= span->firstColumn && x < span->endColumn;
}

// HDR: the kept B tiles' median per sample (hdr->bMedian), which anchors B's own estimate. One frame alone
// would let its hot pixel or glitch win wherever A is clipped.
static void hdr_b_median(HdrScratch *hdr, int keptCount)
{
	for (int y = 0; y < MERGE_TILE_SIZE; y++) {
		for (int x = 0; x < MERGE_TILE_SIZE; x++) {
			int i = y * MERGE_TILE_SIZE + x;
			for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
				float values[MERGE_MAX_FRAMES];
				int count = 0;
				for (int k = 0; k < keptCount; k++) {
					if (!span_contains(&hdr->span[k], y, x) || hdr->keptBClipped[k][plane][i]) {
						continue;
					}
					insert_sorted(values, count, hdr->keptBSamples[k][plane][i]);
					count++;
				}
				if (!count) {
					continue;
				}
				hdr->bMedian[plane][i] = median_of_sorted(values, count);
				hdr->medianMissing[plane][i] = 0;
			}
		}
	}
}

// HDR: B's own estimate (hdr->weightedSumB, hdr->weightSumB) from the kept tiles weighted against their
// median, and the median's agreement with the reference (hdr->agree).
static void hdr_b_estimate(const MergeContext *context, TileScratch *scratch, HdrScratch *hdr, int keptCount)
{
	for (int i = 0; i < TILE_SAMPLES; i++) {
		hdr->bVarianceAtMedian[i] = 0;
	}
	for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
		box_mean_3x3(hdr->bMedian[plane], scratch->scratchPlane);
		for (int i = 0; i < TILE_SAMPLES; i++) {
			hdr->bVarianceAtMedian[i] +=
			        PLANE_SHARE * noise_variance_b(context, plane, scratch->scratchPlane[i]);
		}
	}
	smooth_in_place(hdr->bVarianceAtMedian, scratch->scratchPlane);
	for (int i = 0; i < TILE_SAMPLES; i++) {
		hdr->sampleVariance[i] = scratch->referenceVariance[i] + hdr->bVarianceAtReference[i];
	}
	hdr_robust_weights(scratch, scratch->referenceSamples, hdr->bMedian, true, NULL, hdr->medianMissing,
	                   hdr->sampleVariance, 1.0f, hdr->agree, NULL);
	// Against the median, a tile's difference has B's noise once plus the median's own.
	const float varianceScale = 1.0f + MEDIAN_VARIANCE_FACTOR / (float)keptCount;
	for (int k = 0; k < keptCount; k++) {
		const SampleSpan *span = &hdr->span[k];
		hdr_robust_weights(scratch, hdr->bMedian, hdr->keptBSamples[k], false, hdr->medianMissing,
		                   hdr->keptBClipped[k], hdr->bVarianceAtMedian, varianceScale, hdr->weights, NULL);
		for (int y = span->firstRow; y < span->endRow; y++) {
			for (int x = span->firstColumn; x < span->endColumn; x++) {
				int i = y * MERGE_TILE_SIZE + x;
				float weight = hdr->keptBVerification[k] * hdr->weights[i];
				for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
					if (!hdr->keptBClipped[k][plane][i]) {
						hdr->weightedSumB[plane][i] += weight * hdr->keptBSamples[k][plane][i];
						hdr->weightSumB[plane][i] += weight;
					}
				}
			}
		}
	}
}

// HDR, unclipped reference: A's estimate blended into B's by A's share. The share rises to 1 where the B
// median disagrees with A, which means a ghost or a misregistration in B.
static float unclipped_quotient(const TileScratch *scratch, const HdrScratch *hdr, int plane, int i,
                                float denominatorB)
{
	float quotient = scratch->weightedSum[plane][i] / scratch->weightSum[plane][i];
	if (denominatorB > 0) {
		float shareA = hdr->aShare[i];
		float disagreement = 1.0f - hdr->agree[i];
		if (disagreement > shareA) {
			shareA = disagreement;
		}
		quotient = shareA * quotient + (1.0f - shareA) * (hdr->weightedSumB[plane][i] / denominatorB);
	}
	return quotient;
}

// HDR, clipped reference: B's estimate. B below A's clip by more than their noise contradicts A: B sees
// something else there, such as an object in front of what A sees. Otherwise A's clip is the floor, because a
// misregistered B reads darker.
static float clipped_quotient(const MergeContext *context, const HdrScratch *hdr, int plane, int i,
                              float reference, float denominatorB)
{
	float blackA = context->job->blackA[plane];
	float estimateB = hdr->weightedSumB[plane][i] / denominatorB;
	float weightB = denominatorB > 1 ? denominatorB : 1;
	float varianceB = noise_variance_b(context, plane, reference) / weightB;
	float varianceA =
	        merge_noise_variance(reference, blackA, context->shotNoiseScaleA, context->readNoiseVarianceA);
	float noiseLimit = HDR_FLOOR_SIGMAS * sqrtf(varianceB + varianceA);
	float lowest = reference - noiseLimit - HDR_FLOOR_TOLERANCE * (reference - blackA);
	if (estimateB < lowest) {
		return HDR_MISS;
	}
	if (estimateB < reference) {
		return reference;
	}
	return estimateB;
}

static float hdr_quotient(const MergeContext *context, const TileScratch *scratch, const HdrScratch *hdr,
                          bool needB, int plane, int i)
{
	float reference = scratch->referenceSamples[plane][i];
	float denominatorB = needB ? hdr->weightSumB[plane][i] : 0;
	float quotient;
	if (reference < (float)INPUT_CLIP) {
		quotient = unclipped_quotient(scratch, hdr, plane, i, denominatorB);
	} else if (denominatorB > 0) {
		quotient = clipped_quotient(context, hdr, plane, i, reference, denominatorB);
	} else {
		quotient = HDR_MISS;
	}
	// Noise took this sample over the clip while its plane's 3x3 mean is below the ramp's end: keep A's clip.
	if (quotient == HDR_MISS && reference >= (float)INPUT_CLIP &&
	    hdr->referenceLocalMean[plane][i] < HDR_RAMP_END) {
		quotient = reference;
	}
	return quotient;
}

// HDR: the tile's quotients. Without a usable B for one clipped plane, the whole position has no value, its
// unclipped planes too. The overlapping tiles that have B decide it, and where none has, it goes white in all
// planes. A value in some planes only would show as blotches or colour fringes.
static void hdr_quotients(const MergeContext *context, TileScratch *scratch, const HdrScratch *hdr,
                          bool needB)
{
	for (int i = 0; i < TILE_SAMPLES; i++) {
		bool miss = false;
		for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
			float quotient = hdr_quotient(context, scratch, hdr, needB, plane, i);
			if (quotient == HDR_MISS) {
				miss = true;
			}
			scratch->weightedSum[plane][i] = quotient;
			scratch->weightSum[plane][i] = 1.0f;
		}
		if (miss) {
			for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
				scratch->weightedSum[plane][i] = HDR_MISS;
			}
		}
	}
}

void merge_tiles_hdr(MergeContext *context, TileScratch *scratch, HdrScratch *hdr, int tileY, int tileX)
{
	int clippedCount = gather_reference(context, scratch, tileY, tileX);
	bool countsForUsage = counts_for_usage(clippedCount);
	hdr_reference_noise(context, scratch, hdr);
	bool shareBelowOne = hdr_a_share(hdr);
	// B's own estimate is needed only where A's share is below 1 or the reference is clipped.
	bool needB = clippedCount > 0 || shareBelowOne;
	hdr_merge_alts_a(context, scratch, hdr, tileY, tileX, countsForUsage);
	int keptCount = hdr_merge_alts_b(context, scratch, hdr, tileY, tileX, countsForUsage, needB);
	hdr_prepare_b_estimate(scratch, hdr, needB);
	if (keptCount > 0) {
		hdr_b_median(hdr, keptCount);
		hdr_b_estimate(context, scratch, hdr, keptCount);
	}
	hdr_quotients(context, scratch, hdr, needB);
}
