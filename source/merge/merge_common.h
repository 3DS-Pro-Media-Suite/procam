// Constants, types and helpers shared by the merge stages. Internal to the merge library.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "merge.h"
#include "raw10.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define BAYER_PLANE_COUNT MERGE_CFA_CELLS // one plane per CFA cell
#define PLANE_WIDTH (MERGE_WIDTH / 2)
#define PLANE_HEIGHT (MERGE_HEIGHT / 2)

#define TILE_COUNT (MERGE_TILE_ROWS * MERGE_TILE_COLUMNS)

// A sample from this level up counts as clipped.
#define INPUT_CLIP (RAW10_WHITE - 1)

// Offsets and positions hold (y, x).
enum {
	AXIS_Y,
	AXIS_X,
	AXIS_COUNT
};

// Offsets are stored in steps of 1/SUBPIXEL_STEPS plane pixel.
#define SUBPIXEL_STEPS 8
#define PYRAMID_LEVELS 3
#define MAX_ALT_FRAMES (2 * MERGE_MAX_FRAMES)

enum {
	// Camera B's search is centred on this horizontal offset in plane pixels, the middle of the stereo band.
	B_INITIAL_DX = 36,
	// The reference is the sharpest of camera A's first frames.
	REFERENCE_CANDIDATES = 3,
	// Camera B is dropped when fewer of its tiles match.
	B_MIN_TILES = 64
};

enum {
	// Camera B's per-tile DC match needs at least this many samples.
	HDR_DC_MIN_SAMPLES = 64,
	// A B tile is checked against A only over at least this many positions unclipped in A.
	HDR_CHECK_MIN_POSITIONS = 32
};

// A tile quotient with no value.
#define HDR_MISS (-1e30f)

// 1.5 * 2^23: a float this large has no fraction bits, so adding and removing it rounds.
#define MERGE_ROUNDING_BIAS 12582912.0f

// A grey image pyramid, finest level first, with each level's integral image.
typedef struct {
	int32_t *levels[PYRAMID_LEVELS];
	int32_t *integrals[PYRAMID_LEVELS];
	int widths[PYRAMID_LEVELS];
	int heights[PYRAMID_LEVELS];
} Pyramid;

typedef struct {
	const uint16_t *mosaic;
	// Per tile (y, x) offset in subpixel steps.
	int16_t (*offsets)[MERGE_TILE_COLUMNS][AXIS_COUNT];
	// Camera B only: levels mapped to A's, and a per-tile DC match.
	bool levelsMapped;
	bool dcMatched;
} AltFrame;

static inline int merge_floor_div(int numerator, int denominator)
{
	return numerator >= 0 ? numerator / denominator : -((-numerator + denominator - 1) / denominator);
}

// Rounds half to even like rintf, for |value| < 2^22 in the default rounding mode. The 3DS's VFPv2 has no
// rounding instruction, so rintf would be a library call.
static inline float merge_round_small(float value)
{
	const float bias = MERGE_ROUNDING_BIAS;
	return (value + bias) - bias;
}

// Planes are numbered row by row inside the 2x2 Bayer quad.
static inline float merge_plane_sample(const uint16_t *mosaic, int plane, int y, int x)
{
	int quadRow = plane >> 1;
	int quadColumn = plane & 1;
	return (float)mosaic[(2 * y + quadRow) * MERGE_WIDTH + 2 * x + quadColumn];
}

static inline float merge_noise_variance(float value, float black, float shotNoiseScale,
                                         float readNoiseVariance)
{
	float signal = value - black;
	return shotNoiseScale * (signal > 0 ? signal : 0.0f) + readNoiseVariance;
}

// Rows [firstRow, endRow) and columns [firstColumn, endColumn) of a tile.
typedef struct {
	int firstRow;
	int endRow;
	int firstColumn;
	int endColumn;
} SampleSpan;

// One tile's planes for the merge.
typedef struct {
	// The reference's and the alt frame's samples.
	float referenceSamples[BAYER_PLANE_COUNT][MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	float altSamples[BAYER_PLANE_COUNT][MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// The robust mean's weighted sums and weights.
	float weightedSum[BAYER_PLANE_COUNT][MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	float weightSum[BAYER_PLANE_COUNT][MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// The reference's noise variance.
	float referenceVariance[MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	float scratchPlane[MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// The mean difference and the mean squared difference over the planes, and the latter's 3x3 mean.
	float meanDifference[MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	float meanSquaredDifference[MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	float smoothedSquaredDifference[MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// The alt frame's clipped samples.
	uint8_t altClipped[BAYER_PLANE_COUNT][MERGE_TILE_SIZE * MERGE_TILE_SIZE];
} TileScratch;

// One tile's extra planes for an HDR merge.
typedef struct {
	// The reference's 3x3 means.
	float referenceLocalMean[BAYER_PLANE_COUNT][MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// B's weight in A's estimate: A's noise variance over B's mapped one.
	float bWeightInA[BAYER_PLANE_COUNT][MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// B's mapped noise at the reference's level and at the B median's.
	float bVarianceAtReference[MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	float bVarianceAtMedian[MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// A's share of the blend, and the B median's weight against the reference.
	float aShare[MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	float agree[MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// The B frames' median per sample, which B's own estimate is anchored to.
	float bMedian[BAYER_PLANE_COUNT][MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// B's own estimate.
	float weightedSumB[BAYER_PLANE_COUNT][MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	float weightSumB[BAYER_PLANE_COUNT][MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	float sampleVariance[MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	float weights[MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	float looseWeights[MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// Samples with no median: no unclipped B frame sees them.
	uint8_t medianMissing[BAYER_PLANE_COUNT][MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// The B frames kept for B's estimate, mapped to A's levels.
	float keptBSamples[MERGE_MAX_FRAMES][BAYER_PLANE_COUNT][MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// Their clipped samples.
	uint8_t keptBClipped[MERGE_MAX_FRAMES][BAYER_PLANE_COUNT][MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// Their rows and columns inside B's view.
	SampleSpan span[MERGE_MAX_FRAMES];
	// Their verification against A.
	float keptBVerification[MERGE_MAX_FRAMES];
} HdrScratch;

// One thread's buffers.
typedef struct {
	// The alt frame's grey pyramid.
	Pyramid pyramid;
	// The alignment's work buffer (ALIGN_WORK_INTS) and its integer level 0 offsets (TILE_COUNT x
	// AXIS_COUNT).
	int32_t *work;
	int32_t *integerOffsets;
	// The shake model's tile weights (TILE_COUNT).
	double *affineWeights;
	TileScratch *scratch;
	HdrScratch *hdrScratch;
} MergeWorker;

// One tile row's upper halves: per tile, all planes of MERGE_TILE_STRIDE x MERGE_TILE_SIZE quotients.
#define STASH_FLOATS (MERGE_TILE_COLUMNS * BAYER_PLANE_COUNT * MERGE_TILE_STRIDE * MERGE_TILE_SIZE)

typedef struct MergeContext MergeContext;
typedef void (*MergeItemFn)(MergeContext *context, MergeWorker *worker, int item);

struct MergeContext {
	MergeJob *job;
	// Threads with buffers; the caller is worker 0.
	int threadCount;
	// The most threads that took part in a stage.
	int threadsUsed;
	MergeWorker workers[MERGE_MAX_THREADS];
	// The current stage runs items 0..count-1; next is the next one to hand out (atomic).
	MergeItemFn item;
	int count;
	int next;
	// Progress: items done of total, mapped to base..base + 50 %.
	int done;
	int total;
	int base;
	// An item ran out of memory.
	bool failed;
	uint16_t *framesA[MERGE_MAX_FRAMES];
	uint16_t *framesB[MERGE_MAX_FRAMES];
	// Per frame, summed in order afterwards.
	int fixedDefects[2 * MERGE_MAX_FRAMES];
	int64_t sharp[REFERENCE_CANDIDATES];
	int referenceIndex;
	// Camera A's alt frames, frameCountA - 1. They come first in alts and offsets.
	int altCountA;
	// Camera A's noise model: variance = a * level above black + b.
	float shotNoiseScaleA;
	float readNoiseVarianceA;
	// Camera A's mean black level.
	double blackMeanA;
	// Read-only once built.
	Pyramid referencePyramid;
	int16_t (*offsets)[MERGE_TILE_ROWS][MERGE_TILE_COLUMNS][AXIS_COUNT];
	int32_t *offsetsB[MERGE_MAX_FRAMES];
	bool *goodTilesB[MERGE_MAX_FRAMES];
	// The first alignment item is camera B's first pass.
	bool bFirstPass;
	// Its level ratio scratch (PLANE_WIDTH x PLANE_HEIGHT), allocated before the extra threads' buffers.
	double *levelRatioScratch;
	// Maps camera B's levels to camera A's.
	float levelGain[BAYER_PLANE_COUNT];
	float levelOffset[BAYER_PLANE_COUNT];
	AltFrame alts[MAX_ALT_FRAMES];
	int altCount;
	// Overlap-add output: weighted sums, their weights and the tile window.
	float *accumulatedSum;
	float *accumulatedWeight;
	float tileWindow[MERGE_TILE_SIZE * MERGE_TILE_SIZE];
	// Per tile row, summed in row order afterwards.
	double rowUse[MERGE_TILE_ROWS][MAX_ALT_FRAMES];
	int rowUseCount[MERGE_TILE_ROWS][MAX_ALT_FRAMES];
	// Per row: done flag, the hand-over count of its stash and the stash slot it took.
	int rowDone[MERGE_TILE_ROWS];
	int stashHandOvers[MERGE_TILE_ROWS];
	int slot[MERGE_TILE_ROWS];
	// stashCount slots of STASH_FLOATS.
	float *stash;
	int stashCount;
	int stashBusy[2 * MERGE_MAX_THREADS];
	uint16_t *mosaic;

	// An HDR merge. Cleared when camera B is dropped, which leaves a plain merge of camera A.
	bool hdr;
	// The exposure ratio given.
	float exposureRatio;
	// The output's white in A's units.
	float hdrWhiteLevel;
	// Camera B's noise model at its own gain.
	float shotNoiseScaleB;
	float readNoiseVarianceB;
	// Camera B's known gain: the exposure ratio times its own sensitivity. Only the offset is fitted.
	float fixedGainB[BAYER_PLANE_COUNT];
	// The data's level ratio against that gain.
	double gainCheckB[BAYER_PLANE_COUNT];
	// Camera B's first frame's level ratio to the reference from their histograms; 0 when none.
	double histogramRatioB;
	// Per tile: the reference is not (nearly) all clipped there.
	uint8_t tileInformative[TILE_COUNT];
	// Per plane sample: the covering tiles without a value, one bit per plane.
	uint16_t *miss;
	// Per finish item, summed in order afterwards.
	int finishAboveCount[PLANE_HEIGHT / MERGE_TILE_STRIDE];
	int finishWhiteCount[PLANE_HEIGHT / MERGE_TILE_STRIDE];
};
