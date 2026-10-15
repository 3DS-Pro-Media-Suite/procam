#include "merge_align.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "math_util.h"
#include "merge_common.h"

#define COST_MAX INT64_MAX

#define CANDIDATE_INTS (TILE_COUNT * ALIGN_UPSAMPLE_CANDIDATES * AXIS_COUNT)

// A parabola vertex further away than half a pixel belongs to a neighbouring integer offset.
#define MAX_SUBPIXEL_SHIFT 0.5

// The shake model is a robust affine fit: offset = c0 + c1 * x + c2 * y around the plane's centre.
#define AFFINE_TERMS 3
#define AFFINE_ITERATIONS 4
#define AFFINE_MIN_DETERMINANT 1e-9
// Tiles this close to the model, in subpixel steps, take the model's offset.
#define AFFINE_SNAP_DISTANCE (SUBPIXEL_STEPS / 2)

// Sharpness leaves out an eighth of the frame on each side.
#define SHARPNESS_TOP (PLANE_HEIGHT / 8)
#define SHARPNESS_BOTTOM (PLANE_HEIGHT * 7 / 8)
#define SHARPNESS_LEFT (PLANE_WIDTH / 8)
#define SHARPNESS_RIGHT (PLANE_WIDTH * 7 / 8)

// A tile with at least this many clipped positions in the reference carries no offset information.
#define HDR_FLAT_CLIPPED (3 * MERGE_TILE_SIZE * MERGE_TILE_SIZE / 4)
// An uninformative tile takes camera B's offset from at least this many informative tiles around it.
#define HDR_FILL_MIN_TILES 12
// A weighted mean, then passes with Tukey weights on the residuals.
#define HDR_FILL_ITERATIONS 4

// Tukey cut-offs per pass, in plane pixels.
static const double AFFINE_CUTOFFS[AFFINE_ITERATIONS] = {3.0, 1.5, 1.0, 1.0};
// Tukey cut-offs of the passes after the first, in subpixel steps.
static const double HDR_FILL_CUTOFFS[HDR_FILL_ITERATIONS - 1] = {8.0 * SUBPIXEL_STEPS, 4.0 * SUBPIXEL_STEPS,
                                                                 2.0 * SUBPIXEL_STEPS};

// Finest level first. Camera A looks for hand shake in every direction; camera B searches a wide horizontal
// band for the stereo disparity.
const AlignLevel ALIGN_LEVELS_A[PYRAMID_LEVELS] = {
        {.tileSize = 16, .tileStep = 8, .radiusX = 1, .radiusY = 1, .metric = ALIGN_METRIC_L1},
        {.tileSize = 16, .tileStep = 8, .radiusX = 2, .radiusY = 2, .metric = ALIGN_METRIC_L2},
        {.tileSize = 8, .tileStep = 4, .radiusX = 4, .radiusY = 4, .metric = ALIGN_METRIC_L2},
};
const AlignLevel ALIGN_LEVELS_B[PYRAMID_LEVELS] = {
        {.tileSize = 16, .tileStep = 8, .radiusX = 1, .radiusY = 1, .metric = ALIGN_METRIC_L1},
        {.tileSize = 16, .tileStep = 8, .radiusX = 2, .radiusY = 2, .metric = ALIGN_METRIC_L2},
        {.tileSize = 8, .tileStep = 4, .radiusX = 13, .radiusY = 2, .metric = ALIGN_METRIC_L2},
};

void merge_align_grey_a(const uint16_t *mosaic, int32_t *grey)
{
	for (int y = 0; y < PLANE_HEIGHT; y++) {
		const uint16_t *topRow = mosaic + 2 * y * MERGE_WIDTH;
		const uint16_t *bottomRow = topRow + MERGE_WIDTH;
		for (int x = 0; x < PLANE_WIDTH; x++) {
			grey[y * PLANE_WIDTH + x] =
			        topRow[2 * x] + topRow[2 * x + 1] + bottomRow[2 * x] + bottomRow[2 * x + 1];
		}
	}
}

// Clips each plane at white and sums the planes in float in a fixed order.
void merge_align_grey_b(const uint16_t *mosaic, const float *gain, const float *offset, const float *blackA,
                        const float *blackB, int32_t *grey)
{
	for (int y = 0; y < PLANE_HEIGHT; y++) {
		for (int x = 0; x < PLANE_WIDTH; x++) {
			float sum = 0;
			for (int plane = 0; plane < BAYER_PLANE_COUNT; plane++) {
				float value = merge_plane_sample(mosaic, plane, y, x) - blackB[plane];
				value = gain[plane] * value;
				value = value + offset[plane];
				value = value + blackA[plane];
				if (value > (float)RAW10_WHITE) {
					value = (float)RAW10_WHITE;
				}
				sum = plane ? sum + value : value;
			}
			grey[y * PLANE_WIDTH + x] = (int32_t)merge_round_small(sum);
		}
	}
}

bool merge_align_alloc_pyramid(Pyramid *pyramid)
{
	int width = PLANE_WIDTH;
	int height = PLANE_HEIGHT;
	size_t total = 0;
	for (int level = 0; level < PYRAMID_LEVELS; level++) {
		size_t imageSize = (size_t)width * height;
		size_t integralSize = (size_t)(width + 1) * (height + 1);
		total += imageSize + integralSize;
		width /= 2;
		height /= 2;
	}
	int32_t *buffer = malloc(total * sizeof(int32_t));
	if (!buffer) {
		return false;
	}
	width = PLANE_WIDTH;
	height = PLANE_HEIGHT;
	for (int level = 0; level < PYRAMID_LEVELS; level++) {
		pyramid->levels[level] = buffer;
		buffer += (size_t)width * height;
		pyramid->integrals[level] = buffer;
		buffer += (size_t)(width + 1) * (height + 1);
		pyramid->widths[level] = width;
		pyramid->heights[level] = height;
		width /= 2;
		height /= 2;
	}
	return true;
}

void merge_align_build_pyramid(Pyramid *pyramid)
{
	for (int level = 1; level < PYRAMID_LEVELS; level++) {
		const int32_t *finer = pyramid->levels[level - 1];
		int finerWidth = pyramid->widths[level - 1];
		for (int y = 0; y < pyramid->heights[level]; y++) {
			for (int x = 0; x < pyramid->widths[level]; x++) {
				const int32_t *quad = finer + 2 * y * finerWidth + 2 * x;
				int32_t quadSum = quad[0] + quad[1] + quad[finerWidth] + quad[finerWidth + 1];
				pyramid->levels[level][y * pyramid->widths[level] + x] = quadSum;
			}
		}
	}
	for (int level = 0; level < PYRAMID_LEVELS; level++) {
		int width = pyramid->widths[level];
		int height = pyramid->heights[level];
		int stride = width + 1;
		int32_t *integral = pyramid->integrals[level];
		memset(integral, 0, sizeof(int32_t) * stride);
		for (int y = 0; y < height; y++) {
			int32_t rowSum = 0;
			integral[(y + 1) * stride] = 0;
			for (int x = 0; x < width; x++) {
				rowSum += pyramid->levels[level][y * width + x];
				integral[(y + 1) * stride + x + 1] = integral[y * stride + x + 1] + rowSum;
			}
		}
	}
}

static int32_t box_sum(const Pyramid *pyramid, int level, int top, int left, int size)
{
	const int32_t *integral = pyramid->integrals[level];
	int stride = pyramid->widths[level] + 1;
	int bottom = top + size;
	return integral[bottom * stride + left + size] - integral[top * stride + left + size] -
	       integral[bottom * stride + left] + integral[top * stride + left];
}

// Integer distance of two tiles. Zero-mean costs tolerate camera B's flare; they compare the tiles scaled
// by their pixel count so they stay integer. The result is exact below limit and otherwise some value at or
// above it: partial sums only grow, so a candidate stops as soon as it cannot beat the best so far. Only
// level 0 uses L1, where values are small enough for int32 sums; L2 squares need 64 bits.
static int64_t tile_cost(const int32_t *reference, int referenceStride, const int32_t *alt, int altStride,
                         int size, int metric, bool zeroMean, int32_t sumDifference, int64_t limit)
{
	const int32_t pixelCount = size * size;
	if (metric == ALIGN_METRIC_L1) {
		int32_t sum = 0;
		for (int y = 0; y < size; y++) {
			if (zeroMean) {
				for (int x = 0; x < size; x++) {
					sum += abs(pixelCount * (reference[x] - alt[x]) - sumDifference);
				}
			} else {
				for (int x = 0; x < size; x++) {
					sum += abs(reference[x] - alt[x]);
				}
			}
			if (sum >= limit) {
				return sum;
			}
			reference += referenceStride;
			alt += altStride;
		}
		return sum;
	}
	int64_t squareSum = 0;
	int64_t meanCorrection = zeroMean ? (int64_t)sumDifference * sumDifference : 0;
	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			int32_t difference = reference[x] - alt[x];
			squareSum += (int64_t)difference * difference;
		}
		if ((zeroMean ? pixelCount * squareSum - meanCorrection : squareSum) >= limit) {
			break;
		}
		reference += referenceStride;
		alt += altStride;
	}
	return zeroMean ? pixelCount * squareSum - meanCorrection : squareSum;
}

static int64_t cost_at(const Pyramid *reference, const Pyramid *alt, int level, int top, int left,
                       int offsetY, int offsetX, int size, int metric, bool zeroMean, int64_t limit)
{
	int height = reference->heights[level];
	int width = reference->widths[level];
	int altTop = top + offsetY;
	int altLeft = left + offsetX;
	if (altTop < 0 || altLeft < 0 || altTop + size > height || altLeft + size > width) {
		return COST_MAX;
	}
	int32_t sumDifference = 0;
	if (zeroMean) {
		sumDifference =
		        box_sum(reference, level, top, left, size) - box_sum(alt, level, altTop, altLeft, size);
	}
	const int32_t *referenceTile = reference->levels[level] + top * width + left;
	const int32_t *altTile = alt->levels[level] + altTop * width + altLeft;
	return tile_cost(referenceTile, width, altTile, width, size, metric, zeroMean, sumDifference, limit);
}

static int tiles_across(int length, const AlignLevel *settings)
{
	return (length - settings->tileSize) / settings->tileStep + 1;
}

// candidates: per tile candidateCount (y, x) offsets. The best candidate is searched around; the best offset
// per tile goes to offsets.
static void search_level(const Pyramid *reference, const Pyramid *alt, int level, const AlignLevel *settings,
                         const int32_t *candidates, int candidateCount, bool zeroMean, int32_t *offsets)
{
	int rows = tiles_across(reference->heights[level], settings);
	int columns = tiles_across(reference->widths[level], settings);
	for (int ty = 0; ty < rows; ty++) {
		for (int tx = 0; tx < columns; tx++) {
			int tile = ty * columns + tx;
			const int32_t *tileCandidates = candidates + (size_t)tile * candidateCount * AXIS_COUNT;
			int top = ty * settings->tileStep;
			int left = tx * settings->tileStep;
			int bestCandidate = 0;
			if (candidateCount > 1) {
				int64_t best = COST_MAX;
				for (int k = 0; k < candidateCount; k++) {
					int64_t cost =
					        cost_at(reference, alt, level, top, left, tileCandidates[k * AXIS_COUNT + AXIS_Y],
					                tileCandidates[k * AXIS_COUNT + AXIS_X], settings->tileSize,
					                settings->metric, zeroMean, best);
					if (cost < best) {
						best = cost;
						bestCandidate = k;
					}
				}
			}
			int centreY = tileCandidates[bestCandidate * AXIS_COUNT + AXIS_Y];
			int centreX = tileCandidates[bestCandidate * AXIS_COUNT + AXIS_X];
			int bestY = centreY;
			int bestX = centreX;
			int64_t best = COST_MAX;
			for (int dy = -settings->radiusY; dy <= settings->radiusY; dy++) {
				for (int dx = -settings->radiusX; dx <= settings->radiusX; dx++) {
					int64_t cost = cost_at(reference, alt, level, top, left, centreY + dy, centreX + dx,
					                       settings->tileSize, settings->metric, zeroMean, best);
					if (cost < best) {
						best = cost;
						bestY = centreY + dy;
						bestX = centreX + dx;
					}
				}
			}
			offsets[tile * AXIS_COUNT + AXIS_Y] = bestY;
			offsets[tile * AXIS_COUNT + AXIS_X] = bestX;
		}
	}
}

// The two coarse tiles around a fractional tile position along one axis, nearest first.
static void nearest_two(double position, int count, int *nearest, int *farther)
{
	int below = math_clamp_int((int)floor(position), 0, count - 1);
	int above = math_clamp_int(below + 1, 0, count - 1);
	bool belowIsNearer = position - floor(position) < 0.5;
	*nearest = belowIsNearer ? below : above;
	*farther = belowIsNearer ? above : below;
}

// Candidates for a fine level's tiles: the offsets of the four nearest coarse tiles, doubled, nearest first.
// A tile's position is the fractional index of the coarse tile centred on its own centre.
static void upsample(const int32_t *coarseOffsets, const AlignLevel *coarse, int coarseRows,
                     int coarseColumns, const AlignLevel *fine, int fineHeight, int fineWidth,
                     int32_t *candidates)
{
	int fineRows = tiles_across(fineHeight, fine);
	int fineColumns = tiles_across(fineWidth, fine);
	for (int ty = 0; ty < fineRows; ty++) {
		double rowPosition = ((ty * fine->tileStep + fine->tileSize / 2.0) / 2.0 - coarse->tileSize / 2.0) /
		                     coarse->tileStep;
		int nearRow;
		int farRow;
		nearest_two(rowPosition, coarseRows, &nearRow, &farRow);
		for (int tx = 0; tx < fineColumns; tx++) {
			double columnPosition =
			        ((tx * fine->tileStep + fine->tileSize / 2.0) / 2.0 - coarse->tileSize / 2.0) /
			        coarse->tileStep;
			int nearColumn;
			int farColumn;
			nearest_two(columnPosition, coarseColumns, &nearColumn, &farColumn);
			const int sources[ALIGN_UPSAMPLE_CANDIDATES][AXIS_COUNT] = {
			        {nearRow, nearColumn}, {nearRow, farColumn}, {farRow, nearColumn}, {farRow, farColumn}};
			int32_t *tileCandidates =
			        candidates + (size_t)(ty * fineColumns + tx) * ALIGN_UPSAMPLE_CANDIDATES * AXIS_COUNT;
			for (int k = 0; k < ALIGN_UPSAMPLE_CANDIDATES; k++) {
				int sourceTile = sources[k][AXIS_Y] * coarseColumns + sources[k][AXIS_X];
				tileCandidates[k * AXIS_COUNT + AXIS_Y] = coarseOffsets[sourceTile * AXIS_COUNT + AXIS_Y] * 2;
				tileCandidates[k * AXIS_COUNT + AXIS_X] = coarseOffsets[sourceTile * AXIS_COUNT + AXIS_X] * 2;
			}
		}
	}
}

void merge_align_tiles(const Pyramid *reference, const Pyramid *alt, const AlignLevel *levels, bool zeroMean,
                       int initialY, int initialX, int32_t *offsets, int32_t *work)
{
	int32_t *candidates = work;
	int32_t *coarseOffsets = work + CANDIDATE_INTS;
	const AlignLevel *top = &levels[PYRAMID_LEVELS - 1];
	int level = PYRAMID_LEVELS - 1;
	int scale = 1 << level;
	int rows = (reference->heights[level] - top->tileSize) / top->tileStep + 1;
	int columns = (reference->widths[level] - top->tileSize) / top->tileStep + 1;
	for (int i = 0; i < rows * columns; i++) {
		candidates[i * AXIS_COUNT + AXIS_Y] = merge_floor_div(initialY, scale);
		candidates[i * AXIS_COUNT + AXIS_X] = merge_floor_div(initialX, scale);
	}
	search_level(reference, alt, level, top, candidates, 1, zeroMean, coarseOffsets);
	for (level = PYRAMID_LEVELS - 2; level >= 0; level--) {
		const AlignLevel *coarse = &levels[level + 1];
		const AlignLevel *fine = &levels[level];
		int coarseRows = (reference->heights[level + 1] - coarse->tileSize) / coarse->tileStep + 1;
		int coarseColumns = (reference->widths[level + 1] - coarse->tileSize) / coarse->tileStep + 1;
		upsample(coarseOffsets, coarse, coarseRows, coarseColumns, fine, reference->heights[level],
		         reference->widths[level], candidates);
		int32_t *levelOffsets = level ? coarseOffsets : offsets;
		search_level(reference, alt, level, fine, candidates, ALIGN_UPSAMPLE_CANDIDATES, zeroMean,
		             levelOffsets);
	}
}

// The full L2 cost of a merge tile at level 0.
static int64_t level0_cost(const Pyramid *reference, const Pyramid *alt, int top, int left, int offsetY,
                           int offsetX, bool zeroMean)
{
	return cost_at(reference, alt, 0, top, left, offsetY, offsetX, MERGE_TILE_SIZE, ALIGN_METRIC_L2, zeroMean,
	               COST_MAX);
}

// Each axis gets a parabola through the level 0 costs one pixel either side.
void merge_align_subpixel(const Pyramid *reference, const Pyramid *alt, const int32_t *offsets, bool zeroMean,
                          int16_t (*subpixelOffsets)[MERGE_TILE_COLUMNS][AXIS_COUNT])
{
	for (int ty = 0; ty < MERGE_TILE_ROWS; ty++) {
		for (int tx = 0; tx < MERGE_TILE_COLUMNS; tx++) {
			int tile = ty * MERGE_TILE_COLUMNS + tx;
			int offsetY = offsets[tile * AXIS_COUNT + AXIS_Y];
			int offsetX = offsets[tile * AXIS_COUNT + AXIS_X];
			int top = ty * MERGE_TILE_STRIDE;
			int left = tx * MERGE_TILE_STRIDE;
			int64_t centre = level0_cost(reference, alt, top, left, offsetY, offsetX, zeroMean);
			int result[AXIS_COUNT] = {offsetY * SUBPIXEL_STEPS, offsetX * SUBPIXEL_STEPS};
			for (int axis = 0; axis < AXIS_COUNT; axis++) {
				// One pixel before and after along this axis.
				int64_t before = level0_cost(reference, alt, top, left, offsetY - (axis == AXIS_Y),
				                             offsetX - (axis == AXIS_X), zeroMean);
				int64_t after = level0_cost(reference, alt, top, left, offsetY + (axis == AXIS_Y),
				                            offsetX + (axis == AXIS_X), zeroMean);
				if (centre == COST_MAX || before == COST_MAX || after == COST_MAX) {
					continue;
				}
				double beforeCost = (double)before;
				double afterCost = (double)after;
				double centreCost = (double)centre;
				double curvature = beforeCost - 2 * centreCost + afterCost;
				if (!(curvature > 0)) {
					continue;
				}
				double vertex = 0.5 * (beforeCost - afterCost) / curvature;
				if (vertex < -MAX_SUBPIXEL_SHIFT) {
					vertex = -MAX_SUBPIXEL_SHIFT;
				} else if (vertex > MAX_SUBPIXEL_SHIFT) {
					vertex = MAX_SUBPIXEL_SHIFT;
				}
				result[axis] += (int)rint(vertex * SUBPIXEL_STEPS);
			}
			subpixelOffsets[ty][tx][AXIS_Y] = (int16_t)result[AXIS_Y];
			subpixelOffsets[ty][tx][AXIS_X] = (int16_t)result[AXIS_X];
		}
	}
}

static double determinant_3x3(double m[3][3])
{
	return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
	       m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
	       m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
}

// Cramer's rule.
static void solve_3x3(double matrix[3][3], double determinant, const double *rightSide, double *solution)
{
	for (int k = 0; k < 3; k++) {
		double replaced[3][3];
		memcpy(replaced, matrix, sizeof replaced);
		for (int i = 0; i < 3; i++) {
			replaced[i][k] = rightSide[i];
		}
		solution[k] = determinant_3x3(replaced) / determinant;
	}
}

// Tukey's biweight: 1 at no residual, falling to 0 at the cut-off.
static double tukey_weight(double residual, double cutoff)
{
	double scaled = residual / cutoff;
	double falloff = 1.0 - scaled * scaled;
	return fabs(residual) < cutoff ? falloff * falloff : 0.0;
}

// A tile's centre relative to the plane's centre, in plane pixels.
static double tile_centre_x(int tx)
{
	return (tx * MERGE_TILE_STRIDE + MERGE_TILE_SIZE / 2.0) - PLANE_WIDTH / 2.0;
}

static double tile_centre_y(int ty)
{
	return (ty * MERGE_TILE_STRIDE + MERGE_TILE_SIZE / 2.0) - PLANE_HEIGHT / 2.0;
}

static double affine_at(const double model[AFFINE_TERMS], double x, double y)
{
	return model[0] + model[1] * x + model[2] * y;
}

// Each pass is a weighted least squares fit of both offset axes, then new weights from the distances to it.
// A still burst snaps to 0 and a shaken one to its shake; tiles on a moving subject keep their own offsets.
void merge_align_affine_snap(int16_t (*offsets)[MERGE_TILE_COLUMNS][AXIS_COUNT], double *weightBuffer)
{
	double (*weights)[MERGE_TILE_COLUMNS] = (double (*)[MERGE_TILE_COLUMNS])weightBuffer;
	double modelY[AFFINE_TERMS] = {0};
	double modelX[AFFINE_TERMS] = {0};
	for (int ty = 0; ty < MERGE_TILE_ROWS; ty++) {
		for (int tx = 0; tx < MERGE_TILE_COLUMNS; tx++) {
			weights[ty][tx] = 1.0;
		}
	}
	for (int iteration = 0; iteration < AFFINE_ITERATIONS; iteration++) {
		double normal[AFFINE_TERMS][AFFINE_TERMS] = {{0}};
		double rightY[AFFINE_TERMS] = {0};
		double rightX[AFFINE_TERMS] = {0};
		for (int ty = 0; ty < MERGE_TILE_ROWS; ty++) {
			for (int tx = 0; tx < MERGE_TILE_COLUMNS; tx++) {
				double weight = weights[ty][tx];
				if (weight <= 0) {
					continue;
				}
				double terms[AFFINE_TERMS] = {1.0, tile_centre_x(tx), tile_centre_y(ty)};
				double offsetY = (double)offsets[ty][tx][AXIS_Y] / SUBPIXEL_STEPS;
				double offsetX = (double)offsets[ty][tx][AXIS_X] / SUBPIXEL_STEPS;
				for (int i = 0; i < AFFINE_TERMS; i++) {
					for (int k = 0; k < AFFINE_TERMS; k++) {
						normal[i][k] += weight * terms[i] * terms[k];
					}
					rightY[i] += weight * terms[i] * offsetY;
					rightX[i] += weight * terms[i] * offsetX;
				}
			}
		}
		double determinant = determinant_3x3(normal);
		if (fabs(determinant) < AFFINE_MIN_DETERMINANT) {
			return;
		}
		solve_3x3(normal, determinant, rightY, modelY);
		solve_3x3(normal, determinant, rightX, modelX);
		for (int ty = 0; ty < MERGE_TILE_ROWS; ty++) {
			for (int tx = 0; tx < MERGE_TILE_COLUMNS; tx++) {
				double centreX = tile_centre_x(tx);
				double centreY = tile_centre_y(ty);
				double errorY = (double)offsets[ty][tx][AXIS_Y] / SUBPIXEL_STEPS -
				                affine_at(modelY, centreX, centreY);
				double errorX = (double)offsets[ty][tx][AXIS_X] / SUBPIXEL_STEPS -
				                affine_at(modelX, centreX, centreY);
				double distance = sqrt(errorY * errorY + errorX * errorX);
				weights[ty][tx] = tukey_weight(distance, AFFINE_CUTOFFS[iteration]);
			}
		}
	}
	for (int ty = 0; ty < MERGE_TILE_ROWS; ty++) {
		for (int tx = 0; tx < MERGE_TILE_COLUMNS; tx++) {
			double centreX = tile_centre_x(tx);
			double centreY = tile_centre_y(ty);
			int modelStepsY = (int)rint(affine_at(modelY, centreX, centreY) * SUBPIXEL_STEPS);
			int modelStepsX = (int)rint(affine_at(modelX, centreX, centreY) * SUBPIXEL_STEPS);
			if (abs(offsets[ty][tx][AXIS_Y] - modelStepsY) <= AFFINE_SNAP_DISTANCE &&
			    abs(offsets[ty][tx][AXIS_X] - modelStepsX) <= AFFINE_SNAP_DISTANCE) {
				offsets[ty][tx][AXIS_Y] = (int16_t)modelStepsY;
				offsets[ty][tx][AXIS_X] = (int16_t)modelStepsX;
			}
		}
	}
}

// The sum of absolute central differences over the frame's centre.
int64_t merge_align_sharpness(const int32_t *grey)
{
	int64_t sum = 0;
	for (int y = SHARPNESS_TOP; y < SHARPNESS_BOTTOM; y++) {
		for (int x = SHARPNESS_LEFT; x < SHARPNESS_RIGHT; x++) {
			int32_t gradientX = grey[y * PLANE_WIDTH + x + 1] - grey[y * PLANE_WIDTH + x - 1];
			int32_t gradientY = grey[(y + 1) * PLANE_WIDTH + x] - grey[(y - 1) * PLANE_WIDTH + x];
			sum += abs(gradientX) + abs(gradientY);
		}
	}
	return sum;
}

// The largest sample of the 2x2 Bayer quad at column x.
static int quad_max(const uint16_t *topRow, const uint16_t *bottomRow, int x)
{
	int topMax = topRow[2 * x] > topRow[2 * x + 1] ? topRow[2 * x] : topRow[2 * x + 1];
	int bottomMax = bottomRow[2 * x] > bottomRow[2 * x + 1] ? bottomRow[2 * x] : bottomRow[2 * x + 1];
	return topMax > bottomMax ? topMax : bottomMax;
}

// Any clipped plane makes a position clipped.
void merge_align_hdr_info(const uint16_t *reference, uint8_t *informative)
{
	for (int ty = 0; ty < MERGE_TILE_ROWS; ty++) {
		for (int tx = 0; tx < MERGE_TILE_COLUMNS; tx++) {
			int clipped = 0;
			for (int y = 0; y < MERGE_TILE_SIZE; y++) {
				const uint16_t *topRow = reference + 2 * (ty * MERGE_TILE_STRIDE + y) * MERGE_WIDTH +
				                         2 * tx * MERGE_TILE_STRIDE;
				const uint16_t *bottomRow = topRow + MERGE_WIDTH;
				for (int x = 0; x < MERGE_TILE_SIZE; x++) {
					clipped += quad_max(topRow, bottomRow, x) >= INPUT_CLIP;
				}
			}
			informative[ty * MERGE_TILE_COLUMNS + tx] = clipped < HDR_FLAT_CLIPPED;
		}
	}
}

static bool is_informative(const uint8_t *informative, int ty, int tx)
{
	return ty >= 0 && tx >= 0 && ty < MERGE_TILE_ROWS && tx < MERGE_TILE_COLUMNS &&
	       informative[ty * MERGE_TILE_COLUMNS + tx];
}

// The radius of the smallest square around a tile that holds enough informative tiles.
static int fill_radius(const uint8_t *informative, int tileY, int tileX, int informativeCount)
{
	int radius = 1;
	for (; radius < MERGE_TILE_COLUMNS; radius++) {
		int found = 0;
		for (int y = tileY - radius; y <= tileY + radius; y++) {
			for (int x = tileX - radius; x <= tileX + radius; x++) {
				found += is_informative(informative, y, x);
			}
		}
		if (found >= HDR_FILL_MIN_TILES || found == informativeCount) {
			break;
		}
	}
	return radius;
}

// A robust mean of the informative tiles' offsets minus the shake, the nearest weighing most.
static double neighbour_mean(int16_t (*offsets)[MERGE_TILE_COLUMNS][AXIS_COUNT], const uint8_t *informative,
                             const int16_t *shake, int axis, int tileY, int tileX, int radius)
{
	double mean = 0;
	for (int iteration = 0; iteration < HDR_FILL_ITERATIONS; iteration++) {
		double weightSum = 0;
		double valueSum = 0;
		for (int y = tileY - radius; y <= tileY + radius; y++) {
			for (int x = tileX - radius; x <= tileX + radius; x++) {
				if (!is_informative(informative, y, x)) {
					continue;
				}
				double difference = offsets[y][x][axis] - shake[y * MERGE_TILE_COLUMNS + x];
				int dx = x - tileX;
				int dy = y - tileY;
				double weight = 1.0 / (dx * dx + dy * dy + 1.0);
				if (iteration > 0) {
					weight *= tukey_weight(difference - mean, HDR_FILL_CUTOFFS[iteration - 1]);
				}
				weightSum += weight;
				valueSum += weight * difference;
			}
		}
		if (weightSum <= 0) {
			break;
		}
		mean = valueSum / weightSum;
	}
	return mean;
}

// Where the reference is (nearly) all clipped, both grey images are flat white and the search keeps whichever
// offset came first. Such a tile takes the matching camera A frame's shake (camera B moves with it) plus a
// robust mean of the rest, the rig's offset and the disparity, over the informative tiles around it. Every B
// frame follows the same rule, so they agree with each other where only they see the scene.
void merge_align_hdr_fill(int16_t (*offsets)[MERGE_TILE_COLUMNS][AXIS_COUNT], const uint8_t *informative,
                          const int16_t *shakeY, const int16_t *shakeX, int16_t *scratch)
{
	int informativeCount = 0;
	for (int i = 0; i < TILE_COUNT; i++) {
		informativeCount += informative[i];
	}
	if (!informativeCount || informativeCount == TILE_COUNT) {
		return;
	}
	for (int i = 0; i < TILE_COUNT; i++) {
		if (informative[i]) {
			continue;
		}
		int tileY = i / MERGE_TILE_COLUMNS;
		int tileX = i % MERGE_TILE_COLUMNS;
		int radius = fill_radius(informative, tileY, tileX, informativeCount);
		for (int axis = 0; axis < AXIS_COUNT; axis++) {
			const int16_t *shake = axis == AXIS_X ? shakeX : shakeY;
			double mean = neighbour_mean(offsets, informative, shake, axis, tileY, tileX, radius);
			scratch[i * AXIS_COUNT + axis] = (int16_t)rint(shake[i] + mean);
		}
	}
	for (int i = 0; i < TILE_COUNT; i++) {
		if (!informative[i]) {
			offsets[i / MERGE_TILE_COLUMNS][i % MERGE_TILE_COLUMNS][AXIS_Y] =
			        scratch[i * AXIS_COUNT + AXIS_Y];
			offsets[i / MERGE_TILE_COLUMNS][i % MERGE_TILE_COLUMNS][AXIS_X] =
			        scratch[i * AXIS_COUNT + AXIS_X];
		}
	}
}
