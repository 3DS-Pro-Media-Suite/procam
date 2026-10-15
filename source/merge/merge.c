// Multi-frame raw merge (see merge.h). Portable C11 plus GCC's __atomic builtins, no 3DS calls.
// Build with -ffp-contract=off, so every product is rounded as written.
#include "merge.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "math_util.h"
#include "merge_align.h"
#include "merge_common.h"
#include "merge_defects.h"
#include "merge_levels.h"
#include "merge_stages.h"
#include "merge_tiles.h"
#include "time_units.h"

#define PLANE_SAMPLES (PLANE_WIDTH * PLANE_HEIGHT)
#define HALF_TILE_SAMPLES (MERGE_TILE_STRIDE * MERGE_TILE_SIZE)
#define BAND_SAMPLES (MERGE_TILE_STRIDE * PLANE_WIDTH) // a band is MERGE_TILE_STRIDE plane rows
#define BAND_COUNT (PLANE_HEIGHT / MERGE_TILE_STRIDE)

// Noise model: a sample's variance is slope x (value - black) + floor, both set by the gain code.
#define NOISE_SLOPE_PER_GAIN 0.01508f
#define NOISE_FLOOR_SD_PER_GAIN 0.0386f
#define NOISE_FLOOR_MIN 0.914f

// The alignment fills the progress from 0 to 50 %, the merge from 50 to 100 %.
#define PROGRESS_ALIGN_START 0
#define PROGRESS_MERGE_START MERGE_STAGE_PERCENT
#define PROGRESS_DONE 100

#define STASH_SLOTS_PER_THREAD 2
#define FIRST_PASS_FRAMES 1 // camera B's first pass fits the levels on one frame

// A plain merge drops camera B when a fitted gain falls outside this range.
#define B_GAIN_MIN 0.6f
#define B_GAIN_MAX 2.0f

// HDR drops camera B when the data's own level ratio is this far off the given gain, either way.
#define HDR_RATIO_TOLERANCE 1.5
// HDR drops camera B when its offset exceeds this share of camera A's range plus this many DN of camera B
// at its gain.
#define HDR_OFFSET_MAX_RANGE_SHARE 0.1f
#define HDR_OFFSET_MAX_B_DN 3.0f

#define MOSAIC_MAX 65535.0f // the largest value a mosaic sample holds
#define MIN_WEIGHT_SUM 1e-6f // keeps a division by a window weight sum finite
// HDR: a sample is clipped when its valued tiles weigh less than this share of all its tiles. It is a share
// because a frame corner's only tile weighs about 1e-4.
#define MIN_VALUED_WEIGHT_SHARE 1e-3f

// Each plane sample is covered by 2 x 2 tiles. The HDR miss mask holds a bit per plane and covering tile.
#define COVERING_TILES_ACROSS (MERGE_TILE_SIZE / MERGE_TILE_STRIDE)
#define MISS_BITS_PER_PLANE (COVERING_TILES_ACROSS * COVERING_TILES_ACROSS)
#define MISS_PLANE_MASK ((1 << MISS_BITS_PER_PLANE) - 1)

static void append_log(MergeJob *job, const char *format, ...)
{
	if (!job->log || !job->logCapacity) {
		return;
	}
	size_t length = strlen(job->log);
	if (length + 1 >= job->logCapacity) {
		return;
	}
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(job->log + length, job->logCapacity - length, format, arguments);
	va_end(arguments);
}

static uint64_t now_us(const MergeJob *job)
{
	return job->clockMicroseconds ? job->clockMicroseconds() : 0;
}

static void set_progress(MergeJob *job, int percent)
{
	if (job->progress) {
		*job->progress = percent;
	}
}

static void unpack_all_frames(MergeJob *job)
{
	if (!job->packed) {
		return;
	}
	for (int i = 0; i < job->frameCountA; i++) {
		merge_defects_unpack(job->framesA[i]);
	}
	for (int i = 0; i < job->frameCountB; i++) {
		merge_defects_unpack(job->framesB[i]);
	}
}

static float noise_slope(int gainCode)
{
	return (float)gainCode * NOISE_SLOPE_PER_GAIN;
}

static float noise_floor(int gainCode)
{
	float floorSd = NOISE_FLOOR_SD_PER_GAIN * (float)gainCode;
	float variance = floorSd * floorSd;
	return variance < NOISE_FLOOR_MIN ? NOISE_FLOOR_MIN : variance;
}

// The mean of a per-cell value, summed in double.
static double cell_mean(const float *values)
{
	return (((double)values[0] + values[1]) + values[2] + values[3]) / MERGE_CFA_CELLS;
}

static bool allocate_worker(MergeWorker *worker, bool hdr)
{
	worker->work = malloc(sizeof(int32_t) * ALIGN_WORK_INTS);
	worker->integerOffsets = malloc(sizeof(int32_t) * TILE_COUNT * AXIS_COUNT);
	worker->affineWeights = malloc(sizeof(double) * TILE_COUNT);
	worker->scratch = malloc(sizeof *worker->scratch);
	worker->hdrScratch = hdr ? malloc(sizeof *worker->hdrScratch) : NULL;
	return worker->work && worker->integerOffsets && worker->affineWeights && worker->scratch &&
	       (worker->hdrScratch || !hdr) && merge_align_alloc_pyramid(&worker->pyramid);
}

// Frees the alignment buffers, which the merge stage does not need.
static void free_worker_alignment(MergeWorker *worker)
{
	free(worker->work);
	free(worker->integerOffsets);
	free(worker->affineWeights);
	free(worker->pyramid.levels[0]); // all levels share one allocation
	worker->work = NULL;
	worker->integerOffsets = NULL;
	worker->affineWeights = NULL;
	worker->pyramid.levels[0] = NULL;
}

static void free_worker(MergeWorker *worker)
{
	free_worker_alignment(worker);
	free(worker->scratch);
	free(worker->hdrScratch);
	worker->scratch = NULL;
	worker->hdrScratch = NULL;
}

// Camera A's alt frames are its frames without the reference.
static int alt_frame_index(const MergeContext *context, int altIndex)
{
	return altIndex < context->referenceIndex ? altIndex : altIndex + 1;
}

// Stage 1, per frame, camera A first: unpack and fix defects. Camera A's first frames are also rated for
// sharpness, to choose the reference.
static void stage_load_frame(MergeContext *context, MergeWorker *worker, int frameIndex)
{
	MergeJob *job = context->job;
	bool fromCameraA = frameIndex < job->frameCountA;
	int cameraIndex = fromCameraA ? frameIndex : frameIndex - job->frameCountA;
	uint8_t *rawFrame = fromCameraA ? job->framesA[cameraIndex] : job->framesB[cameraIndex];
	if (job->packed) {
		merge_defects_unpack(rawFrame);
	}
	uint16_t *frame = (uint16_t *)(void *)rawFrame;
	if (fromCameraA) {
		context->fixedDefects[frameIndex] = merge_defects_fix(frame, job->hotA, job->hotCountA);
	} else {
		context->fixedDefects[frameIndex] = merge_defects_fix(frame, job->hotB, job->hotCountB);
	}
	if (fromCameraA && cameraIndex < REFERENCE_CANDIDATES) {
		merge_align_grey_a(frame, worker->pyramid.levels[0]);
		context->sharp[cameraIndex] = merge_align_sharpness(worker->pyramid.levels[0]);
	}
}

// Camera B's first pass: B_0 is aligned at one global level ratio, and its good tiles give the first
// per-plane level fit.
static void align_b_first_pass(MergeContext *context, MergeWorker *worker)
{
	MergeJob *job = context->job;
	merge_align_grey_a(context->framesB[0], worker->pyramid.levels[0]);
	double levelRatio;
	if (context->hdr) {
		context->histogramRatioB =
		        merge_levels_quantile_ratio(context->referencePyramid.levels[0], worker->pyramid.levels[0],
		                                    job->blackA, job->blackB, context->levelRatioScratch);
		levelRatio = context->histogramRatioB;
		if (!(levelRatio > 0)) {
			levelRatio = context->exposureRatio; // camera A is clipped everywhere: use the ratio given
		}
	} else {
		levelRatio = merge_levels_median_ratio(context->referencePyramid.levels[0], worker->pyramid.levels[0],
		                                       job->blackA, job->blackB, context->levelRatioScratch);
	}
	float uniformGain[MERGE_CFA_CELLS] = {(float)levelRatio, (float)levelRatio, (float)levelRatio,
	                                      (float)levelRatio};
	float zeroOffset[MERGE_CFA_CELLS] = {0, 0, 0, 0};
	merge_align_grey_b(context->framesB[0], uniformGain, zeroOffset, job->blackA, job->blackB,
	                   worker->pyramid.levels[0]);
	merge_align_build_pyramid(&worker->pyramid);
	merge_align_tiles(&context->referencePyramid, &worker->pyramid, ALIGN_LEVELS_B, true, 0, B_INITIAL_DX,
	                  context->offsetsB[0], worker->work);
	const NoiseModelB modelB = {.gain = levelRatio,
	                            .shotNoiseScale = context->shotNoiseScaleB,
	                            .readNoiseVariance = context->readNoiseVarianceB};
	merge_levels_good_tiles(context->referencePyramid.levels[0], worker->pyramid.levels[0],
	                        context->offsetsB[0], context->shotNoiseScaleA, context->readNoiseVarianceA,
	                        context->blackMeanA, context->hdr ? &modelB : NULL, context->goodTilesB[0]);
	const uint16_t *frames[FIRST_PASS_FRAMES] = {context->framesB[0]};
	int32_t *offsets[FIRST_PASS_FRAMES] = {context->offsetsB[0]};
	bool *goodTiles[FIRST_PASS_FRAMES] = {context->goodTilesB[0]};
	int matchedTiles = 0;
	bool fitted = merge_levels_fit(context->framesA[context->referenceIndex], frames, offsets, goodTiles,
	                               FIRST_PASS_FRAMES, job->blackA, job->blackB,
	                               context->hdr ? context->fixedGainB : NULL, context->gainCheckB,
	                               context->levelGain, context->levelOffset, &matchedTiles);
	if (!fitted) {
		context->failed = true;
		return;
	}
	// The only item that logs while others run.
	append_log(job,
	           "B pass 1: ratio %.4f, %d tiles -> gain %.4f %.4f %.4f %.4f, offset %.2f %.2f %.2f %.2f\n",
	           levelRatio, matchedTiles, context->levelGain[0], context->levelGain[1], context->levelGain[2],
	           context->levelGain[3], context->levelOffset[0], context->levelOffset[1],
	           context->levelOffset[2], context->levelOffset[3]);
}

// Stage 2: camera B's first pass when it runs, as the longest item first, then each camera A alt frame.
static void stage_align_camera_a(MergeContext *context, MergeWorker *worker, int itemIndex)
{
	int altIndex = itemIndex;
	if (context->bFirstPass) {
		if (itemIndex == 0) {
			align_b_first_pass(context, worker);
			merge_stages_item_done(context);
			return;
		}
		altIndex--;
	}
	int16_t (*offsets)[MERGE_TILE_COLUMNS][AXIS_COUNT] = context->offsets[altIndex];
	merge_align_grey_a(context->framesA[alt_frame_index(context, altIndex)], worker->pyramid.levels[0]);
	merge_align_build_pyramid(&worker->pyramid);
	merge_align_tiles(&context->referencePyramid, &worker->pyramid, ALIGN_LEVELS_A, false, 0, 0,
	                  worker->integerOffsets, worker->work);
	merge_align_subpixel(&context->referencePyramid, &worker->pyramid, worker->integerOffsets, false,
	                     offsets);
	merge_align_affine_snap(offsets, worker->affineWeights);
	merge_stages_item_done(context);
}

// HDR: camera A_k's offsets against the reference, in 1/8 plane pixels. B_k shares that hand shake, because
// the two cameras are one rigid rig with synchronised frames. Zero for the reference itself and for a B frame
// without an A partner.
static void hdr_shake(const MergeContext *context, int frameIndex, int16_t *shakeY, int16_t *shakeX)
{
	int altIndex = -1;
	if (frameIndex != context->referenceIndex && frameIndex < context->job->frameCountA) {
		altIndex = frameIndex < context->referenceIndex ? frameIndex : frameIndex - 1;
	}
	for (int ty = 0; ty < MERGE_TILE_ROWS; ty++) {
		for (int tx = 0; tx < MERGE_TILE_COLUMNS; tx++) {
			int tile = ty * MERGE_TILE_COLUMNS + tx;
			shakeY[tile] = altIndex >= 0 ? context->offsets[altIndex][ty][tx][AXIS_Y] : 0;
			shakeX[tile] = altIndex >= 0 ? context->offsets[altIndex][ty][tx][AXIS_X] : 0;
		}
	}
}

// Stage 3, per camera B frame: the stereo band search, with the levels mapped by the first pass's fit.
static void stage_align_camera_b(MergeContext *context, MergeWorker *worker, int frameIndex)
{
	MergeJob *job = context->job;
	int altIndex = context->altCountA + frameIndex;
	merge_align_grey_b(context->framesB[frameIndex], context->levelGain, context->levelOffset, job->blackA,
	                   job->blackB, worker->pyramid.levels[0]);
	merge_align_build_pyramid(&worker->pyramid);
	merge_align_tiles(&context->referencePyramid, &worker->pyramid, ALIGN_LEVELS_B, true, 0, B_INITIAL_DX,
	                  context->offsetsB[frameIndex], worker->work);
	const NoiseModelB modelB = {.gain = cell_mean(context->levelGain),
	                            .shotNoiseScale = context->shotNoiseScaleB,
	                            .readNoiseVariance = context->readNoiseVarianceB};
	merge_levels_good_tiles(context->referencePyramid.levels[0], worker->pyramid.levels[0],
	                        context->offsetsB[frameIndex], context->shotNoiseScaleA,
	                        context->readNoiseVarianceA, context->blackMeanA, context->hdr ? &modelB : NULL,
	                        context->goodTilesB[frameIndex]);
	merge_align_subpixel(&context->referencePyramid, &worker->pyramid, context->offsetsB[frameIndex], true,
	                     context->offsets[altIndex]);
	if (context->hdr) {
		// The alignment's work buffer is free again; a worker's stack is too small for these.
		int16_t *fillScratch = (int16_t *)(void *)worker->work;
		int16_t *shakeY = fillScratch + TILE_COUNT * AXIS_COUNT;
		int16_t *shakeX = shakeY + TILE_COUNT;
		hdr_shake(context, frameIndex, shakeY, shakeX);
		merge_align_hdr_fill(context->offsets[altIndex], context->tileInformative, shakeY, shakeX,
		                     fillScratch);
	}
	merge_stages_item_done(context);
}

// Index of a plane sample in the accumulator, which holds the planes one after another.
static size_t accumulator_index(int plane, size_t sample)
{
	return (size_t)plane * PLANE_HEIGHT * PLANE_WIDTH + sample;
}

// Index of a quotient in a row's stash, which holds the upper half of each tile and plane.
static int stash_index(int tileX, int plane, int sampleInTile)
{
	return (tileX * BAYER_PLANE_COUNT + plane) * HALF_TILE_SAMPLES + sampleInTile;
}

// Index in a plane of the sample at (y, x) of a tile.
static size_t tile_sample_index(int tileY, int tileX, int y, int x)
{
	return (size_t)(tileY * MERGE_TILE_STRIDE + y) * PLANE_WIDTH + tileX * MERGE_TILE_STRIDE + x;
}

// Index in the RGGB mosaic of a plane sample.
static int mosaic_index(int plane, int y, int x)
{
	int cellRow = plane >> 1;
	int cellColumn = plane & 1;
	return (2 * y + cellRow) * MERGE_WIDTH + 2 * x + cellColumn;
}

// Takes a free stash slot for a tile row. One is always free: a stash lives while its row or the row above
// runs, so at most 2 x threads - 1 are in use when a row asks. If another thread takes the slot a scan is
// heading for, the scan goes round again.
static float *take_stash_slot(MergeContext *context, int row)
{
	for (;;) {
		for (int slot = 0; slot < context->stashCount; slot++) {
			if (!__atomic_exchange_n(&context->stashBusy[slot], 1, __ATOMIC_ACQUIRE)) {
				context->slot[row] = slot;
				return context->stash + (size_t)slot * STASH_FLOATS;
			}
		}
	}
}

// HDR: adds a tile's quotient for one plane sample, weighted by the window. A missing quotient sets the
// sample's miss bit for this plane and covering tile instead, so finishing can take its weight back out.
static inline void add_hdr_quotient(MergeContext *context, int plane, size_t sample, int y, int x,
                                    float quotient, float weight)
{
	if (quotient == HDR_MISS) {
		int lowerHalf = y >= MERGE_TILE_STRIDE;
		int rightHalf = x >= MERGE_TILE_STRIDE;
		int bit = plane * MISS_BITS_PER_PLANE + lowerHalf * COVERING_TILES_ACROSS + rightHalf;
		context->miss[sample] |= (uint16_t)(1u << bit);
	} else {
		context->accumulatedSum[accumulator_index(plane, sample)] += quotient * weight;
	}
}

// Adds a row's stashed upper half in the order merge_tile_row adds it directly, then frees the slot.
static void fold_stash(MergeContext *context, int row)
{
	const float *stash = context->stash + (size_t)context->slot[row] * STASH_FLOATS;
	const float *window = context->tileWindow;
	for (int tx = 0; tx < MERGE_TILE_COLUMNS; tx++) {
		for (int y = 0; y < MERGE_TILE_STRIDE; y++) {
			for (int x = 0; x < MERGE_TILE_SIZE; x++) {
				int i = y * MERGE_TILE_SIZE + x;
				size_t sample = tile_sample_index(row, tx, y, x);
				if (context->hdr) {
					for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
						add_hdr_quotient(context, plane, sample, y, x, stash[stash_index(tx, plane, i)],
						                 window[i]);
					}
				} else {
					for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
						context->accumulatedSum[accumulator_index(plane, sample)] +=
						        stash[stash_index(tx, plane, i)] * window[i];
					}
				}
				context->accumulatedWeight[sample] += window[i];
			}
		}
	}
	__atomic_store_n(&context->stashBusy[context->slot[row]], 0, __ATOMIC_RELEASE);
}

// Clears the bands a tile row writes first: its lower band, and for row 0 its upper band too.
static void clear_row_sums(float *sums, float *weightSums, int row)
{
	for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
		float *planeSums = sums + accumulator_index(plane, 0);
		if (!row) {
			memset(planeSums, 0, sizeof(float) * BAND_SAMPLES);
		}
		memset(planeSums + (size_t)(row + 1) * BAND_SAMPLES, 0, sizeof(float) * BAND_SAMPLES);
	}
	if (!row) {
		memset(weightSums, 0, sizeof(float) * BAND_SAMPLES);
	}
	memset(weightSums + (size_t)(row + 1) * BAND_SAMPLES, 0, sizeof(float) * BAND_SAMPLES);
}

// Stage 4, one row of merge tiles, overlap-added into the sums and weight sums in the single-thread order,
// because float sums depend on their order. Band ty gets row ty - 1's lower half first, then row ty's upper
// half. While row ty - 1 still runs, row ty keeps its upper half in a stash, and whichever of the two ends
// second folds it in. The lower half always goes straight in: the row below adds to it only after this row is
// done.
static void merge_tile_row(MergeContext *context, MergeWorker *worker, int row)
{
	TileScratch *scratch = worker->scratch;
	const float *window = context->tileWindow;
	float *sums = context->accumulatedSum;
	float *weightSums = context->accumulatedWeight;
	bool rowAboveRunning = row && !__atomic_load_n(&context->rowDone[row - 1], __ATOMIC_SEQ_CST);
	float *stash = rowAboveRunning ? take_stash_slot(context, row) : NULL;
	clear_row_sums(sums, weightSums, row);
	for (int alt = 0; alt < context->altCount; alt++) {
		context->rowUse[row][alt] = 0;
		context->rowUseCount[row][alt] = 0;
	}
	for (int tx = 0; tx < MERGE_TILE_COLUMNS; tx++) {
		if (context->hdr) {
			merge_tiles_hdr(context, scratch, worker->hdrScratch, row, tx);
		} else {
			merge_tiles_plain(context, scratch, row, tx);
		}
		for (int y = 0; y < MERGE_TILE_SIZE; y++) {
			for (int x = 0; x < MERGE_TILE_SIZE; x++) {
				int i = y * MERGE_TILE_SIZE + x;
				if (stash && y < MERGE_TILE_STRIDE) {
					for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
						stash[stash_index(tx, plane, i)] =
						        scratch->weightedSum[plane][i] / scratch->weightSum[plane][i];
					}
					continue;
				}
				size_t sample = tile_sample_index(row, tx, y, x);
				if (context->hdr) {
					for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
						add_hdr_quotient(context, plane, sample, y, x,
						                 scratch->weightedSum[plane][i] / scratch->weightSum[plane][i],
						                 window[i]);
					}
				} else {
					for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
						sums[accumulator_index(plane, sample)] +=
						        (scratch->weightedSum[plane][i] / scratch->weightSum[plane][i]) * window[i];
					}
				}
				weightSums[sample] += window[i];
			}
		}
	}
	// A row's stash is counted by the row above when it ends and by its own row when it ends; the second
	// count folds it in. A row without a stash never counts its own, so only the row above counts.
	if (stash && __atomic_fetch_add(&context->stashHandOvers[row], 1, __ATOMIC_ACQ_REL) == 1) {
		fold_stash(context, row);
	}
	__atomic_store_n(&context->rowDone[row], 1, __ATOMIC_SEQ_CST);
	if (row + 1 < MERGE_TILE_ROWS &&
	    __atomic_fetch_add(&context->stashHandOvers[row + 1], 1, __ATOMIC_ACQ_REL) == 1) {
		fold_stash(context, row + 1);
	}
}

static void stage_merge_row(MergeContext *context, MergeWorker *worker, int row)
{
	merge_tile_row(context, worker, row);
	merge_stages_item_done(context);
}

// HDR: the window weight of the tiles that cover a plane sample but had no value for it.
static float missing_weight(const MergeContext *context, int plane, size_t sample, int y, int x)
{
	int missMask = context->miss[sample] >> (plane * MISS_BITS_PER_PLANE) & MISS_PLANE_MASK;
	float weight = 0;
	for (int bit = 0; missMask; bit++) {
		if (missMask & 1) {
			int lowerHalf = bit >> 1;
			int rightHalf = bit & 1;
			int windowY = y % MERGE_TILE_STRIDE + lowerHalf * MERGE_TILE_STRIDE;
			int windowX = x % MERGE_TILE_STRIDE + rightHalf * MERGE_TILE_STRIDE;
			weight += context->tileWindow[windowY * MERGE_TILE_SIZE + windowX];
		}
		missMask >>= 1;
	}
	return weight;
}

// Scales a plane sample into the mosaic, rounded and clamped to the mosaic's white.
static void store_mosaic_sample(MergeContext *context, int plane, int y, int x, float value,
                                float mosaicWhite)
{
	float scaled = merge_round_small(value * (float)MERGE_SCALE);
	context->mosaic[mosaic_index(plane, y, x)] = (uint16_t)math_clamp_float(scaled, 0, mosaicWhite);
}

// Stage 5 of an HDR merge, per band: normalised, capped at white and scaled into the mosaic. No reference
// pixel is kept: the tiles already gave clipped ones camera B's estimate.
static void finish_hdr_band(MergeContext *context, int band)
{
	const float white = context->hdrWhiteLevel;
	const float mosaicWhite = white * (float)MERGE_SCALE; // exact: the mosaic's white is an integer
	int aboveCount = 0;
	int whiteCount = 0;
	int firstRow = band * MERGE_TILE_STRIDE;
	for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
		for (int y = firstRow; y < firstRow + MERGE_TILE_STRIDE; y++) {
			for (int x = 0; x < PLANE_WIDTH; x++) {
				size_t sample = (size_t)y * PLANE_WIDTH + x;
				float missingWeightSum = missing_weight(context, plane, sample, y, x);
				float valuedWeightSum = context->accumulatedWeight[sample] - missingWeightSum;
				float allWeightSum = valuedWeightSum + missingWeightSum;
				bool clipped =
				        missingWeightSum > 0 && valuedWeightSum <= MIN_VALUED_WEIGHT_SHARE * allWeightSum;
				float value = white;
				if (!clipped) {
					float weightSum = valuedWeightSum > MIN_WEIGHT_SUM ? valuedWeightSum : MIN_WEIGHT_SUM;
					value = context->accumulatedSum[accumulator_index(plane, sample)] * (1.0f / weightSum);
				}
				value = value < white ? value : white;
				if (value > (float)RAW10_WHITE) {
					aboveCount++;
				}
				if (value >= white) {
					whiteCount++;
				}
				store_mosaic_sample(context, plane, y, x, value, mosaicWhite);
			}
		}
	}
	context->finishAboveCount[band] = aboveCount;
	context->finishWhiteCount[band] = whiteCount;
}

// Stage 5, per band: normalised and scaled into the mosaic. Clipped reference pixels stay as they are.
static void stage_finish_band(MergeContext *context, MergeWorker *worker, int band)
{
	(void)worker;
	if (context->hdr) {
		finish_hdr_band(context, band);
		return;
	}
	const uint16_t *reference = context->framesA[context->referenceIndex];
	int firstRow = band * MERGE_TILE_STRIDE;
	for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
		for (int y = firstRow; y < firstRow + MERGE_TILE_STRIDE; y++) {
			for (int x = 0; x < PLANE_WIDTH; x++) {
				size_t sample = (size_t)y * PLANE_WIDTH + x;
				float weightSum = context->accumulatedWeight[sample] > MIN_WEIGHT_SUM
				                          ? context->accumulatedWeight[sample]
				                          : MIN_WEIGHT_SUM;
				float merged = context->accumulatedSum[accumulator_index(plane, sample)] * (1.0f / weightSum);
				float referenceValue = merge_plane_sample(reference, plane, y, x);
				float value;
				if (referenceValue >= (float)INPUT_CLIP) {
					value = referenceValue;
				} else {
					value = merged < (float)RAW10_WHITE ? merged : (float)RAW10_WHITE;
				}
				store_mosaic_sample(context, plane, y, x, value, MERGE_SCALE * RAW10_WHITE);
			}
		}
	}
}

static void init_context(MergeContext *context, MergeJob *job)
{
	context->job = job;
	context->hdr = job->hdr && job->frameCountB > 0;
	context->exposureRatio = job->hdrRatio > 0 ? job->hdrRatio : 1.0f;
	context->hdrWhiteLevel = RAW10_WHITE; // set from camera B's levels once they are fitted
	int gainB = job->gainB > 0 ? job->gainB : job->gain;
	context->shotNoiseScaleB = noise_slope(gainB);
	context->readNoiseVarianceB = noise_floor(gainB);
	for (int cell = 0; cell < MERGE_CFA_CELLS; cell++) {
		float sensitivity =
		        job->hdrSensitivityB[cell] > 0 ? job->hdrSensitivityB[cell] : MERGE_HDR_SENSITIVITY;
		context->fixedGainB[cell] = context->exposureRatio * sensitivity;
	}
}

// Camera B's tile offsets and good tiles, and the scratch of its first pass.
static bool allocate_camera_b(MergeContext *context)
{
	MergeJob *job = context->job;
	for (int i = 0; i < job->frameCountB; i++) {
		context->offsetsB[i] = malloc(sizeof(int32_t) * TILE_COUNT * AXIS_COUNT);
		if (!context->offsetsB[i]) {
			return false;
		}
		context->goodTilesB[i] = malloc(TILE_COUNT);
		if (!context->goodTilesB[i]) {
			return false;
		}
	}
	if (!job->frameCountB) {
		return true;
	}
	context->levelRatioScratch = malloc(sizeof(double) * PLANE_WIDTH * PLANE_HEIGHT);
	return context->levelRatioScratch != NULL;
}

// The extra threads' buffers, as many as fit: short of memory, the merge runs on fewer threads rather than
// failing. Camera B's first pass allocates for its level fit during the alignment, so that room is held here.
static void allocate_extra_workers(MergeContext *context, int wantedThreads)
{
	void *levelFitRoom = context->levelRatioScratch ? malloc(LEVELS_FIT_BYTES(FIRST_PASS_FRAMES)) : NULL;
	while (context->threadCount < wantedThreads &&
	       allocate_worker(&context->workers[context->threadCount], context->hdr)) {
		context->threadCount++;
	}
	if (context->threadCount < wantedThreads) {
		free_worker(&context->workers[context->threadCount]); // the one that did not fit
	}
	free(levelFitRoom);
}

// What every merge needs comes first: the caller's worker, the offsets, the reference pyramid and camera B's
// buffers. Extra threads only get what is left.
static bool allocate_alignment(MergeContext *context)
{
	MergeJob *job = context->job;
	int wantedThreads = job->spawn && job->join ? math_clamp_int(job->threads, 1, MERGE_MAX_THREADS) : 1;
	if (!allocate_worker(&context->workers[0], context->hdr)) {
		return false;
	}
	context->threadCount = 1;
	context->offsets = malloc(sizeof *context->offsets * MAX_ALT_FRAMES);
	if (!context->offsets || !merge_align_alloc_pyramid(&context->referencePyramid)) {
		return false;
	}
	if (!allocate_camera_b(context)) {
		return false;
	}
	allocate_extra_workers(context, wantedThreads);
	return true;
}

// Stage 1, then the reference: the sharpest of camera A's first frames.
static void load_frames(MergeContext *context, MergeResult *result)
{
	MergeJob *job = context->job;
	context->shotNoiseScaleA = noise_slope(job->gain);
	context->readNoiseVarianceA = noise_floor(job->gain);
	context->blackMeanA = cell_mean(job->blackA);
	for (int i = 0; i < job->frameCountA; i++) {
		context->framesA[i] = (uint16_t *)(void *)job->framesA[i];
	}
	for (int i = 0; i < job->frameCountB; i++) {
		context->framesB[i] = (uint16_t *)(void *)job->framesB[i];
	}
	merge_stages_run(context, job->frameCountA + job->frameCountB, stage_load_frame);
	for (int i = 0; i < job->frameCountA + job->frameCountB; i++) {
		result->fixedDefects += context->fixedDefects[i];
	}
	int64_t sharpest = -1;
	for (int i = 0; i < job->frameCountA && i < REFERENCE_CANDIDATES; i++) {
		if (context->sharp[i] > sharpest) {
			sharpest = context->sharp[i];
			context->referenceIndex = i;
		}
	}
	result->referenceIndex = context->referenceIndex;
	merge_align_grey_a(context->framesA[context->referenceIndex], context->referencePyramid.levels[0]);
	merge_align_build_pyramid(&context->referencePyramid);
	append_log(job, "merge: %d + %d frames, gain %d (noise %.4f, %.3f), %d defects fixed, reference A_%02d\n",
	           job->frameCountA, job->frameCountB, job->gain, context->shotNoiseScaleA,
	           context->readNoiseVarianceA, result->fixedDefects, context->referenceIndex);
}

// HDR: marks the tiles the reference has (nearly) all clipped. Camera B's offsets there are filled in.
static void find_hdr_clipped_tiles(MergeContext *context)
{
	MergeJob *job = context->job;
	merge_align_hdr_info(context->framesA[context->referenceIndex], context->tileInformative);
	int clippedTiles = 0;
	for (int i = 0; i < TILE_COUNT; i++) {
		clippedTiles += !context->tileInformative[i];
	}
	append_log(job,
	           "HDR: camera B %.2f x shorter (gain %d, noise %.4f, %.3f); %d of %d tiles clipped in the "
	           "reference (B offsets filled in)\n",
	           context->exposureRatio, job->gainB > 0 ? job->gainB : job->gain, context->shotNoiseScaleB,
	           context->readNoiseVarianceB, clippedTiles, TILE_COUNT);
}

// The alt frames: camera A's without the reference, then camera B's.
static void set_up_alt_frames(MergeContext *context)
{
	MergeJob *job = context->job;
	context->altCountA = job->frameCountA - 1;
	for (int i = 0; i < context->altCountA; i++) {
		context->alts[i] = (AltFrame){.mosaic = context->framesA[alt_frame_index(context, i)],
		                              .offsets = context->offsets[i],
		                              .levelsMapped = false,
		                              .dcMatched = false};
	}
	for (int k = 0; k < job->frameCountB; k++) {
		context->alts[context->altCountA + k] =
		        (AltFrame){.mosaic = context->framesB[k],
		                   .offsets = context->offsets[context->altCountA + k],
		                   .levelsMapped = true,
		                   .dcMatched = true};
	}
	context->altCount = context->altCountA + job->frameCountB;
}

// Stages 2 and 3. The alignment buffers are freed before the level fit and the merge stage take their own,
// so the peak memory stays near the larger of the two stages. Returns false when out of memory.
static bool align_frames(MergeContext *context)
{
	MergeJob *job = context->job;
	for (int cell = 0; cell < MERGE_CFA_CELLS; cell++) {
		context->levelGain[cell] = 1; // until camera B's first pass fits them
		context->levelOffset[cell] = 0;
	}
	context->bFirstPass = job->frameCountB > 0;
	merge_stages_start_progress(context, context->altCountA + job->frameCountB + context->bFirstPass,
	                            PROGRESS_ALIGN_START);
	merge_stages_run(context, context->bFirstPass + context->altCountA, stage_align_camera_a);
	free(context->levelRatioScratch);
	context->levelRatioScratch = NULL;
	bool ok = !context->failed;
	if (ok && job->frameCountB) {
		merge_stages_run(context, job->frameCountB, stage_align_camera_b);
	}
	for (int k = 0; k < context->threadCount; k++) {
		free_worker_alignment(&context->workers[k]);
	}
	free(context->referencePyramid.levels[0]);
	context->referencePyramid.levels[0] = NULL;
	return ok;
}

static bool ratio_within_tolerance(double ratio)
{
	return ratio >= 1 / HDR_RATIO_TOLERANCE && ratio <= HDR_RATIO_TOLERANCE;
}

// HDR: camera B's gain is given, so the checks are how well the data's own level ratio and B_0's histogram
// ratio agree with it, and the offset. Over a narrow range of levels, a large offset can hide a wrong gain.
static bool hdr_levels_sane(MergeContext *context, int matchedTiles)
{
	MergeJob *job = context->job;
	double histogramRatio =
	        context->histogramRatioB > 0 ? context->histogramRatioB / cell_mean(context->levelGain) : 1.0;
	append_log(
	        job,
	        "HDR: B gain = ratio %.3f x %.4f %.4f %.4f %.4f (B's own sensitivity); the data's level ratio to "
	        "it %.3f %.3f %.3f %.3f, histograms' %.3f\n",
	        context->exposureRatio, context->levelGain[0] / context->exposureRatio,
	        context->levelGain[1] / context->exposureRatio, context->levelGain[2] / context->exposureRatio,
	        context->levelGain[3] / context->exposureRatio, context->gainCheckB[0], context->gainCheckB[1],
	        context->gainCheckB[2], context->gainCheckB[3], histogramRatio);
	if (matchedTiles < B_MIN_TILES || !ratio_within_tolerance(histogramRatio)) {
		return false;
	}
	for (int cell = 0; cell < MERGE_CFA_CELLS; cell++) {
		float rangeShare = HDR_OFFSET_MAX_RANGE_SHARE * (RAW10_WHITE - job->blackA[cell]);
		float maxOffset = rangeShare + HDR_OFFSET_MAX_B_DN * context->levelGain[cell];
		if (!ratio_within_tolerance(context->gainCheckB[cell])) {
			return false;
		}
		if (!(fabsf(context->levelOffset[cell]) <= maxOffset)) {
			return false;
		}
	}
	return true;
}

static bool plain_levels_sane(const MergeContext *context, int matchedTiles)
{
	if (matchedTiles < B_MIN_TILES) {
		return false;
	}
	for (int cell = 0; cell < MERGE_CFA_CELLS; cell++) {
		float gain = context->levelGain[cell];
		if (!(gain >= B_GAIN_MIN && gain <= B_GAIN_MAX)) {
			return false;
		}
	}
	return true;
}

// Camera B's levels over all its frames. When they cannot be matched (a covered lens, lost sync, no
// overlap), camera B is dropped and camera A merged alone. Returns false when out of memory.
static bool fit_camera_b_levels(MergeContext *context)
{
	MergeJob *job = context->job;
	const uint16_t *framesB[MERGE_MAX_FRAMES];
	for (int k = 0; k < job->frameCountB; k++) {
		framesB[k] = context->framesB[k];
	}
	int matchedTiles = 0;
	bool fitted = merge_levels_fit(context->framesA[context->referenceIndex], framesB, context->offsetsB,
	                               context->goodTilesB, job->frameCountB, job->blackA, job->blackB,
	                               context->hdr ? context->fixedGainB : NULL, context->gainCheckB,
	                               context->levelGain, context->levelOffset, &matchedTiles);
	append_log(job, "B levels: %d tiles -> gain %.4f %.4f %.4f %.4f, offset %.2f %.2f %.2f %.2f\n",
	           matchedTiles, context->levelGain[0], context->levelGain[1], context->levelGain[2],
	           context->levelGain[3], context->levelOffset[0], context->levelOffset[1],
	           context->levelOffset[2], context->levelOffset[3]);
	bool sane;
	if (context->hdr) {
		sane = hdr_levels_sane(context, matchedTiles);
	} else {
		sane = plain_levels_sane(context, matchedTiles);
	}
	if (fitted && !sane) {
		append_log(job, "B dropped: its levels could not be matched%s\n",
		           context->hdr ? " (HDR: no extra range)" : "");
		context->altCount -= job->frameCountB;
		context->hdr = false;
	}
	return fitted;
}

// HDR: the output's white is where camera B's own clip lands at its levels, in the brightest plane. It is at
// least camera A's white, at most what the mosaic's 16 bits hold, and an integer once scaled.
static void set_hdr_white(MergeContext *context)
{
	MergeJob *job = context->job;
	float top = (float)RAW10_WHITE;
	float limit = MOSAIC_MAX / (float)MERGE_SCALE;
	for (int cell = 0; cell < MERGE_CFA_CELLS; cell++) {
		float clipLevel = job->blackA[cell] +
		                  context->levelGain[cell] * ((float)INPUT_CLIP - job->blackB[cell]) +
		                  context->levelOffset[cell];
		if (clipLevel > top) {
			top = clipLevel;
		}
	}
	float white = top < limit ? top : limit;
	context->hdrWhiteLevel = rintf(white * (float)MERGE_SCALE) / (float)MERGE_SCALE;
	append_log(job, "HDR: white %d, camera B's clip at its levels (x%.2f of A's)\n",
	           (int)(context->hdrWhiteLevel * MERGE_SCALE), context->hdrWhiteLevel / RAW10_WHITE);
}

// The merge stage's buffers. Without its stash slots, the merge stage runs on one thread.
static bool allocate_merge(MergeContext *context, MergeResult *result)
{
	context->accumulatedSum = malloc(sizeof(float) * BAYER_PLANE_COUNT * PLANE_SAMPLES);
	context->accumulatedWeight = malloc(sizeof(float) * PLANE_SAMPLES);
	result->mosaic = malloc(sizeof(uint16_t) * MERGE_WIDTH * MERGE_HEIGHT);
	context->mosaic = result->mosaic;
	bool ok = context->accumulatedSum && context->accumulatedWeight && context->mosaic;
	if (ok && context->hdr) {
		context->miss = calloc(PLANE_SAMPLES, sizeof(uint16_t));
		ok = context->miss != NULL;
	}
	if (context->threadCount > 1) {
		context->stashCount = STASH_SLOTS_PER_THREAD * context->threadCount;
		context->stash = malloc(sizeof(float) * STASH_FLOATS * context->stashCount);
		if (!context->stash) {
			context->threadCount = 1;
		}
	}
	return ok;
}

// A raised-cosine window: tiles at half their size apart add up to a flat weight.
static void build_tile_window(MergeContext *context)
{
	for (int y = 0; y < MERGE_TILE_SIZE; y++) {
		for (int x = 0; x < MERGE_TILE_SIZE; x++) {
			float weightY =
			        0.5f - 0.5f * cosf((float)(2 * M_PI) * ((float)y + 0.5f) / (float)MERGE_TILE_SIZE);
			float weightX =
			        0.5f - 0.5f * cosf((float)(2 * M_PI) * ((float)x + 0.5f) / (float)MERGE_TILE_SIZE);
			context->tileWindow[y * MERGE_TILE_SIZE + x] = weightY * weightX;
		}
	}
}

// The mean merge weight of one alt frame over all tile rows.
static double alt_frame_use(const MergeContext *context, int alt)
{
	double useSum = 0;
	int useCount = 0;
	for (int row = 0; row < MERGE_TILE_ROWS; row++) {
		useSum += context->rowUse[row][alt];
		useCount += context->rowUseCount[row][alt];
	}
	return useCount ? useSum / useCount : 0;
}

static void report_use(const MergeContext *context, MergeResult *result)
{
	double useSumA = 0;
	double useSumB = 0;
	int countA = 0;
	int countB = 0;
	for (int alt = 0; alt < context->altCount; alt++) {
		double use = alt_frame_use(context, alt);
		if (context->alts[alt].levelsMapped) {
			useSumB += use;
			countB++;
		} else {
			useSumA += use;
			countA++;
		}
	}
	result->useA = countA ? (float)(useSumA / countA) : 0;
	result->useB = countB ? (float)(useSumB / countB) : 0;
}

static void report_hdr(MergeContext *context, MergeResult *result)
{
	int aboveCount = 0;
	int whiteCount = 0;
	for (int band = 0; band < BAND_COUNT; band++) {
		aboveCount += context->finishAboveCount[band];
		whiteCount += context->finishWhiteCount[band];
	}
	result->white = (int)(context->hdrWhiteLevel * MERGE_SCALE);
	result->hdrRatio = context->hdrWhiteLevel / RAW10_WHITE;
	result->hdrUsed = true;
	result->hdrAbove = (float)aboveCount / (BAYER_PLANE_COUNT * PLANE_SAMPLES);
	result->hdrWhite = (float)whiteCount / (BAYER_PLANE_COUNT * PLANE_SAMPLES);
	append_log(context->job,
	           "HDR: white %d (x%.2f); %.2f %% of samples above A's clip, %.3f %% left at white\n",
	           result->white, result->hdrRatio, 100 * result->hdrAbove, 100 * result->hdrWhite);
}

// Stages 4 and 5, then the result's statistics.
static void merge_and_finish(MergeContext *context, MergeResult *result, uint64_t alignEndUs)
{
	MergeJob *job = context->job;
	build_tile_window(context);
	merge_stages_start_progress(context, MERGE_TILE_ROWS, PROGRESS_MERGE_START);
	merge_stages_run(context, MERGE_TILE_ROWS, stage_merge_row);
	merge_stages_run(context, BAND_COUNT, stage_finish_band);
	report_use(context, result);
	result->threads = context->threadsUsed;
	result->mergeMs = (uint32_t)((now_us(job) - alignEndUs) / MICROSECONDS_PER_MILLISECOND);
	if (context->hdr) {
		report_hdr(context, result);
	}
	append_log(job, "merged: weight A %.3f B %.3f; align %u ms, merge %u ms, %d thread%s\n", result->useA,
	           result->useB, result->alignMs, result->mergeMs, context->threadsUsed,
	           context->threadsUsed > 1 ? "s" : "");
}

// Everything after the alignment buffers are taken. Returns false when out of memory.
static bool merge_frames(MergeContext *context, MergeResult *result, uint64_t startUs)
{
	MergeJob *job = context->job;
	load_frames(context, result);
	if (context->hdr) {
		find_hdr_clipped_tiles(context);
	}
	set_up_alt_frames(context);
	bool ok = align_frames(context);
	if (ok && job->frameCountB) {
		ok = fit_camera_b_levels(context);
	}
	if (ok && context->hdr) {
		set_hdr_white(context);
	}
	memcpy(result->levelGain, context->levelGain, sizeof context->levelGain);
	memcpy(result->levelOffset, context->levelOffset, sizeof context->levelOffset);
	uint64_t alignEndUs = now_us(job);
	result->alignMs = (uint32_t)((alignEndUs - startUs) / MICROSECONDS_PER_MILLISECOND);
	if (ok) {
		ok = allocate_merge(context, result);
	}
	if (!ok) {
		return false;
	}
	merge_and_finish(context, result, alignEndUs);
	return true;
}

static void free_context(MergeContext *context)
{
	for (int k = 0; k < MERGE_MAX_THREADS; k++) {
		free_worker(&context->workers[k]);
	}
	free(context->offsets);
	free(context->accumulatedSum);
	free(context->accumulatedWeight);
	free(context->stash);
	free(context->miss);
	free(context->levelRatioScratch);
	free(context->referencePyramid.levels[0]);
	for (int i = 0; i < MERGE_MAX_FRAMES; i++) {
		free(context->offsetsB[i]);
		free(context->goodTilesB[i]);
	}
	free(context);
}

bool merge_run(MergeJob *job, MergeResult *result)
{
	memset(result, 0, sizeof *result);
	result->white = MERGE_SCALE * RAW10_WHITE; // until an HDR merge succeeds
	result->hdrRatio = 1;
	if (job->frameCountA < 1 || job->frameCountA > MERGE_MAX_FRAMES || job->frameCountB < 0 ||
	    job->frameCountB > MERGE_MAX_FRAMES) {
		return false;
	}
	uint64_t startUs = now_us(job);
	set_progress(job, PROGRESS_ALIGN_START);
	MergeContext *context = calloc(1, sizeof *context);
	bool ok = false;
	if (context) {
		init_context(context, job);
		ok = allocate_alignment(context);
	}
	if (ok) {
		ok = merge_frames(context, result, startUs);
	} else {
		unpack_all_frames(job); // the caller's fallback uses one frame as it is, unpacked
	}
	if (context) {
		free_context(context);
	}
	set_progress(job, PROGRESS_DONE);
	if (!ok) {
		free(result->mosaic);
		result->mosaic = NULL;
	}
	return ok;
}
