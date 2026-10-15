#include "merge_levels.h"
#include <math.h>
#include <stdlib.h>
#include "merge_common.h"

#define PLANE_PIXELS (PLANE_WIDTH * PLANE_HEIGHT)

// A good tile's zero-mean residual stays below this many times the expected noise.
#define GOOD_TILE_NOISE_RATIO 2.0

// Tiles with a sample at or above this level may be clipped and stay out of the fit.
#define FIT_MAX_LEVEL 1000
#define FIT_ITERATIONS 5
// Turns a median absolute residual into a standard deviation for normal noise.
#define MAD_TO_SIGMA 1.4826
// Keeps the robust scale and the residuals away from 0.
#define MIN_ROBUST_SCALE 1e-3
#define MIN_RESIDUAL 1e-6
// Residuals beyond this many robust scales get less weight (Huber).
#define HUBER_SCALES 2.0
// Fitted gains and level ratios are rounded to 1/LEVEL_GAIN_STEPS, offsets to 1/LEVEL_OFFSET_STEPS.
#define LEVEL_GAIN_STEPS 4096
#define LEVEL_OFFSET_STEPS 64

// The known gain is checked on tiles at least this far above black, where the offset matters little.
#define GAIN_CHECK_MIN_LEVEL 100.0
// With fewer such tiles the check reports a ratio of 1.
#define GAIN_CHECK_MIN_TILES 16

// Level ratios use the pixels below this level above black, which camera A has not clipped.
#define RATIO_MAX_LEVEL 900

// Marks a reference tile whose statistics are not computed yet.
#define NOT_MEASURED (-1)

typedef struct {
	double gain;
	double offset;
} LevelLine;

static void swap_values(double *first, double *second)
{
	double kept = *first;
	*first = *second;
	*second = kept;
}

// The rank-th smallest of values[0..count). Reorders values so that values[0..rank) <= values[rank] <=
// values[rank+1..count): Hoare partition around a median of three, linear on average.
static double select_rank(double *values, int count, int rank)
{
	int low = 0;
	int high = count - 1;
	while (high > low) {
		int middle = low + (high - low) / 2;
		if (values[middle] < values[low]) {
			swap_values(&values[middle], &values[low]);
		}
		if (values[high] < values[low]) {
			swap_values(&values[high], &values[low]);
		}
		if (values[high] < values[middle]) {
			swap_values(&values[high], &values[middle]);
		}
		double pivot = values[middle];
		int i = low;
		int j = high;
		// values[low] <= pivot <= values[high] stop both scans inside the range.
		while (i <= j) {
			while (values[i] < pivot) {
				i++;
			}
			while (values[j] > pivot) {
				j--;
			}
			if (i <= j) {
				swap_values(&values[i], &values[j]);
				i++;
				j--;
			}
		}
		if (rank <= j) {
			high = j;
		} else if (rank >= i) {
			low = i;
		} else {
			// Between the two parts every value equals the pivot.
			return values[rank];
		}
	}
	return values[rank];
}

// Reorders values. Gives the same value a sort would.
static double median(double *values, int count)
{
	if (count <= 0) {
		return 0;
	}
	double upper = select_rank(values, count, count / 2);
	if (count % 2) {
		return upper;
	}
	// The largest of values[0..count/2) is the rank just below.
	double lower = values[0];
	for (int i = 1; i < count / 2; i++) {
		if (values[i] > lower) {
			lower = values[i];
		}
	}
	return 0.5 * (lower + upper);
}

static double round_to_steps(double value, int stepsPerUnit)
{
	return rint(value * stepsPerUnit) / stepsPerUnit;
}

// The mean absolute value of zero-mean normal noise with this variance.
static double mean_absolute_noise(double variance)
{
	return sqrt(variance) * sqrt(2.0 / M_PI);
}

// Zero-mean residuals ignore a level difference between the cameras. In HDR, camera B's side of the residual
// has its own noise after its gain: gain * shot noise + gain^2 * read noise.
void merge_levels_good_tiles(const int32_t *referenceGrey, const int32_t *altGrey, const int32_t *offsets,
                             double shotNoiseScale, double readNoiseVariance, double blackMeanA,
                             const NoiseModelB *modelB, bool *good)
{
	const int64_t pixelCount = MERGE_TILE_SIZE * MERGE_TILE_SIZE;
	for (int ty = 0; ty < MERGE_TILE_ROWS; ty++) {
		for (int tx = 0; tx < MERGE_TILE_COLUMNS; tx++) {
			int tile = ty * MERGE_TILE_COLUMNS + tx;
			int top = ty * MERGE_TILE_STRIDE;
			int left = tx * MERGE_TILE_STRIDE;
			int altTop = top + offsets[tile * AXIS_COUNT + AXIS_Y];
			int altLeft = left + offsets[tile * AXIS_COUNT + AXIS_X];
			good[tile] = false;
			if (altTop < 0 || altLeft < 0 || altTop + MERGE_TILE_SIZE > PLANE_HEIGHT ||
			    altLeft + MERGE_TILE_SIZE > PLANE_WIDTH) {
				continue;
			}
			int64_t sumDifference = 0;
			int64_t referenceSum = 0;
			int32_t referenceMax = 0;
			for (int y = 0; y < MERGE_TILE_SIZE; y++) {
				for (int x = 0; x < MERGE_TILE_SIZE; x++) {
					int32_t value = referenceGrey[(top + y) * PLANE_WIDTH + left + x];
					sumDifference += value - altGrey[(altTop + y) * PLANE_WIDTH + altLeft + x];
					referenceSum += value;
					if (value > referenceMax) {
						referenceMax = value;
					}
				}
			}
			int64_t absoluteSum = 0;
			for (int y = 0; y < MERGE_TILE_SIZE; y++) {
				for (int x = 0; x < MERGE_TILE_SIZE; x++) {
					int32_t difference = referenceGrey[(top + y) * PLANE_WIDTH + left + x] -
					                     altGrey[(altTop + y) * PLANE_WIDTH + altLeft + x];
					absoluteSum += llabs(pixelCount * difference - sumDifference);
				}
			}
			double meanResidual = (double)absoluteSum / pixelCount / pixelCount / BAYER_PLANE_COUNT;
			double level = (double)referenceSum / pixelCount / BAYER_PLANE_COUNT;
			double signal = fmax(level - blackMeanA, 0.0);
			double varianceA = shotNoiseScale * signal + readNoiseVariance;
			double expectedResidual;
			if (modelB) {
				double gainB = modelB->gain;
				double varianceB =
				        gainB * modelB->shotNoiseScale * signal + gainB * gainB * modelB->readNoiseVariance;
				expectedResidual = mean_absolute_noise((varianceA + varianceB) / BAYER_PLANE_COUNT);
			} else {
				expectedResidual = mean_absolute_noise(2.0 * varianceA / BAYER_PLANE_COUNT);
			}
			good[tile] = meanResidual / expectedResidual < GOOD_TILE_NOISE_RATIO &&
			             referenceMax < BAYER_PLANE_COUNT * INPUT_CLIP;
		}
	}
}

static void tile_mean_max(const uint16_t *mosaic, int plane, int top, int left, double *mean, int *max)
{
	int64_t sum = 0;
	int largest = 0;
	for (int y = 0; y < MERGE_TILE_SIZE; y++) {
		for (int x = 0; x < MERGE_TILE_SIZE; x++) {
			int value = mosaic[(2 * (top + y) + (plane >> 1)) * MERGE_WIDTH + 2 * (left + x) + (plane & 1)];
			sum += value;
			if (value > largest) {
				largest = value;
			}
		}
	}
	*mean = (double)sum / (MERGE_TILE_SIZE * MERGE_TILE_SIZE);
	*max = largest;
}

// Iteratively reweighted least squares of y = gain * x + offset, starting from line. With gainKnown only the
// offset is fitted.
static LevelLine fit_line_robust(const double *x, const double *y, int count, bool gainKnown, LevelLine line,
                                 double *weights, double *residuals, double *scratch)
{
	for (int i = 0; i < count; i++) {
		weights[i] = 1.0;
	}
	for (int iteration = 0; iteration < FIT_ITERATIONS; iteration++) {
		double sumXX = 0;
		double sumX = 0;
		double sumWeights = 0;
		double sumXY = 0;
		double sumY = 0;
		for (int i = 0; i < count; i++) {
			double weight = weights[i] * weights[i];
			sumXX += weight * x[i] * x[i];
			sumX += weight * x[i];
			sumWeights += weight;
			sumXY += weight * x[i] * y[i];
			sumY += weight * y[i];
		}
		if (gainKnown) {
			if (sumWeights <= 0) {
				break;
			}
			line.offset = (sumY - line.gain * sumX) / sumWeights;
		} else {
			double determinant = sumXX * sumWeights - sumX * sumX;
			if (determinant <= 0) {
				break;
			}
			line.gain = (sumXY * sumWeights - sumX * sumY) / determinant;
			line.offset = (sumXX * sumY - sumX * sumXY) / determinant;
		}
		for (int i = 0; i < count; i++) {
			residuals[i] = fabs(y[i] - (line.gain * x[i] + line.offset));
			scratch[i] = residuals[i];
		}
		double scale = MAD_TO_SIGMA * median(scratch, count) + MIN_ROBUST_SCALE;
		for (int i = 0; i < count; i++) {
			double residual = residuals[i] > MIN_RESIDUAL ? residuals[i] : MIN_RESIDUAL;
			double ratio = HUBER_SCALES * scale / residual;
			weights[i] = sqrt(ratio < 1.0 ? ratio : 1.0);
		}
	}
	return line;
}

// The median ratio of y to the line's prediction over the bright tiles. 1 when too few tiles are bright.
static double gain_check_ratio(const double *x, const double *y, int count, LevelLine line, double *ratios)
{
	int ratioCount = 0;
	for (int i = 0; i < count; i++) {
		double predicted = line.gain * x[i] + line.offset;
		if (y[i] >= GAIN_CHECK_MIN_LEVEL && predicted > 0) {
			ratios[ratioCount] = y[i] / predicted;
			ratioCount++;
		}
	}
	return ratioCount >= GAIN_CHECK_MIN_TILES ? median(ratios, ratioCount) : 1.0;
}

// A gain fitted in HDR comes out too flat: the tiles unclipped in A are only its darker ones, and a slope
// over a narrow range of noisy tile means is diluted by the noise. So HDR passes the known gain.
bool merge_levels_fit(const uint16_t *reference, const uint16_t *const *frames, int32_t *const *offsets,
                      bool *const *goodTiles, int frameCount, const float *blackA, const float *blackB,
                      const float *knownGain, double *gainCheck, float *gain, float *offset, int *tileCount)
{
	size_t arrayBytes = sizeof(double) * TILE_COUNT * frameCount;
	// Per tile pair: camera B's and the reference's mean above black.
	double *altLevels = malloc(arrayBytes);
	double *referenceLevels = malloc(arrayBytes);
	double *weights = malloc(arrayBytes);
	double *residuals = malloc(arrayBytes);
	double *scratch = malloc(arrayBytes);
	// The reference tile's statistics, shared by every frame's tile there.
	double *referenceMeans = malloc(sizeof(double) * TILE_COUNT);
	int *referenceMaxima = malloc(sizeof(int) * TILE_COUNT);
	bool ok = altLevels && referenceLevels && weights && residuals && scratch && referenceMeans &&
	          referenceMaxima;
	for (int plane = 0; ok && plane < BAYER_PLANE_COUNT; plane++) {
		int count = 0;
		for (int i = 0; i < TILE_COUNT; i++) {
			referenceMaxima[i] = NOT_MEASURED;
		}
		for (int f = 0; f < frameCount; f++) {
			for (int i = 0; i < TILE_COUNT; i++) {
				if (!goodTiles[f][i]) {
					continue;
				}
				int top = (i / MERGE_TILE_COLUMNS) * MERGE_TILE_STRIDE;
				int left = (i % MERGE_TILE_COLUMNS) * MERGE_TILE_STRIDE;
				if (referenceMaxima[i] == NOT_MEASURED) {
					tile_mean_max(reference, plane, top, left, &referenceMeans[i], &referenceMaxima[i]);
				}
				double altMean;
				int altMax;
				tile_mean_max(frames[f], plane, top + offsets[f][i * AXIS_COUNT + AXIS_Y],
				              left + offsets[f][i * AXIS_COUNT + AXIS_X], &altMean, &altMax);
				if (referenceMaxima[i] >= FIT_MAX_LEVEL || altMax >= FIT_MAX_LEVEL) {
					continue;
				}
				altLevels[count] = altMean - blackB[plane];
				referenceLevels[count] = referenceMeans[i] - blackA[plane];
				count++;
			}
		}
		LevelLine start = {.gain = knownGain ? knownGain[plane] : 1.0, .offset = 0.0};
		LevelLine line = fit_line_robust(altLevels, referenceLevels, count, knownGain != NULL, start, weights,
		                                 residuals, scratch);
		gain[plane] = (float)round_to_steps(line.gain, LEVEL_GAIN_STEPS);
		offset[plane] = (float)round_to_steps(line.offset, LEVEL_OFFSET_STEPS);
		if (knownGain) {
			gainCheck[plane] = gain_check_ratio(altLevels, referenceLevels, count, line, scratch);
		}
		*tileCount = count;
	}
	free(altLevels);
	free(referenceLevels);
	free(weights);
	free(residuals);
	free(scratch);
	free(referenceMeans);
	free(referenceMaxima);
	return ok;
}

// A grey pixel's mean plane level above black.
static double grey_level(int32_t grey, double blackSum)
{
	return grey / (double)BAYER_PLANE_COUNT - blackSum / BAYER_PLANE_COUNT;
}

// Collects the grey levels below RATIO_MAX_LEVEL and returns their count.
static int unclipped_levels(const int32_t *grey, double blackSum, double *levels)
{
	int count = 0;
	for (int i = 0; i < PLANE_PIXELS; i++) {
		double level = grey_level(grey[i], blackSum);
		if (level < RATIO_MAX_LEVEL) {
			levels[count] = level;
			count++;
		}
	}
	return count;
}

double merge_levels_median_ratio(const int32_t *referenceGrey, const int32_t *greyB, const float *blackA,
                                 const float *blackB, double *scratch)
{
	double blackSumA = ((double)blackA[0] + blackA[1] + blackA[2]) + blackA[3];
	double blackSumB = ((double)blackB[0] + blackB[1] + blackB[2]) + blackB[3];
	int count = unclipped_levels(referenceGrey, blackSumA, scratch);
	double medianA = median(scratch, count);
	count = unclipped_levels(greyB, blackSumB, scratch);
	double medianB = median(scratch, count);
	return round_to_steps(medianA / medianB, LEVEL_GAIN_STEPS);
}

// The two views see nearly the same scene, so nearly the same histogram: equal ranks compare the same pixels.
double merge_levels_quantile_ratio(const int32_t *referenceGrey, const int32_t *greyB, const float *blackA,
                                   const float *blackB, double *scratch)
{
	double blackSumA = ((double)blackA[0] + blackA[1] + blackA[2]) + blackA[3];
	double blackSumB = ((double)blackB[0] + blackB[1] + blackB[2]) + blackB[3];
	int count = unclipped_levels(referenceGrey, blackSumA, scratch);
	if (count < 2) {
		return 0;
	}
	double medianA = median(scratch, count);
	for (int i = 0; i < PLANE_PIXELS; i++) {
		scratch[i] = grey_level(greyB[i], blackSumB);
	}
	double sameRankB = select_rank(scratch, PLANE_PIXELS, count / 2);
	return sameRankB > 0 ? round_to_steps(medianA / sameRankB, LEVEL_GAIN_STEPS) : 0;
}
