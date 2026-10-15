// Fits camera B's levels to camera A's per Bayer plane.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "merge_common.h"

// Per frame, merge_levels_fit allocates this many double arrays of TILE_COUNT values.
#define LEVELS_FIT_ARRAYS_PER_FRAME 5
// The working memory merge_levels_fit takes for frameCount frames: those arrays plus one double and one int
// array of TILE_COUNT values.
#define LEVELS_FIT_BYTES(frameCount)                                                          \
	(sizeof(double) * TILE_COUNT * (LEVELS_FIT_ARRAYS_PER_FRAME * (size_t)(frameCount) + 1) + \
	 sizeof(int) * TILE_COUNT)

// HDR: camera B's gain to A's levels and its own noise model.
typedef struct {
	double gain;
	double shotNoiseScale;
	double readNoiseVariance;
} NoiseModelB;

// Marks the camera B tiles good for the level fit: matched within the noise, not clipped, inside the frame.
// modelB is NULL for a plain merge.
void merge_levels_good_tiles(const int32_t *referenceGrey, const int32_t *altGrey, const int32_t *offsets,
                             double shotNoiseScale, double readNoiseVariance, double blackMeanA,
                             const NoiseModelB *modelB, bool *good);
// Fits reference - blackA = gain * (B - blackB) + offset per plane over the good tiles of every frame.
// knownGain is NULL unless the gains are known (HDR): then only the offsets are fitted and gainCheck gets the
// data's own level ratio against them. tileCount gets the number of tiles the last plane used.
bool merge_levels_fit(const uint16_t *reference, const uint16_t *const *frames, int32_t *const *offsets,
                      bool *const *goodTiles, int frameCount, const float *blackA, const float *blackB,
                      const float *knownGain, double *gainCheck, float *gain, float *offset, int *tileCount);
// The ratio of the two grey images' median levels above black. scratch: one plane of doubles.
double merge_levels_median_ratio(const int32_t *referenceGrey, const int32_t *greyB, const float *blackA,
                                 const float *blackB, double *scratch);
// The same for HDR, where camera A clips and camera B does not: the median of A's unclipped pixels against
// B's pixel of the same rank. 0 when A has no unclipped pixels.
double merge_levels_quantile_ratio(const int32_t *referenceGrey, const int32_t *greyB, const float *blackA,
                                   const float *blackB, double *scratch);
