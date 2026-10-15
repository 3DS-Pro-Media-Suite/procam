// Aligns frames to the reference: grey images, tile pyramids, offset search, subpixel and shake model, HDR
// offset fill.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "merge_common.h"

// Distances a pyramid level's search can use.
enum {
	ALIGN_METRIC_L1 = 1,
	ALIGN_METRIC_L2 = 2
};

// Each finer pyramid level tries the offsets of this many nearest coarser tiles.
#define ALIGN_UPSAMPLE_CANDIDATES 4
// Size of merge_align_tiles' work buffer in int32 values: per tile the candidates' (y, x) offsets and the
// coarser level's offset.
#define ALIGN_WORK_INTS (TILE_COUNT * (ALIGN_UPSAMPLE_CANDIDATES + 1) * AXIS_COUNT)

// One pyramid level's tile search.
typedef struct {
	int tileSize;
	int tileStep;
	int radiusX;
	int radiusY;
	int metric;
} AlignLevel;

extern const AlignLevel ALIGN_LEVELS_A[PYRAMID_LEVELS];
extern const AlignLevel ALIGN_LEVELS_B[PYRAMID_LEVELS];

// Sums each 2x2 Bayer quad into one grey pixel.
void merge_align_grey_a(const uint16_t *mosaic, int32_t *grey);
// The same for camera B, with its levels mapped to camera A's first.
void merge_align_grey_b(const uint16_t *mosaic, const float *gain, const float *offset, const float *blackA,
                        const float *blackB, int32_t *grey);
bool merge_align_alloc_pyramid(Pyramid *pyramid);
// Builds the coarser levels and all integral images from a filled level 0.
void merge_align_build_pyramid(Pyramid *pyramid);
// Integer level 0 offsets (TILE_COUNT x 2) of the alt frame against the reference, coarse to fine.
void merge_align_tiles(const Pyramid *reference, const Pyramid *alt, const AlignLevel *levels, bool zeroMean,
                       int initialY, int initialX, int32_t *offsets, int32_t *work);
// Refines integer offsets to subpixel steps.
void merge_align_subpixel(const Pyramid *reference, const Pyramid *alt, const int32_t *offsets, bool zeroMean,
                          int16_t (*subpixelOffsets)[MERGE_TILE_COLUMNS][AXIS_COUNT]);
// Snaps camera A's tile offsets to a robust affine model of the hand shake. weightBuffer: TILE_COUNT doubles.
void merge_align_affine_snap(int16_t (*offsets)[MERGE_TILE_COLUMNS][AXIS_COUNT], double *weightBuffer);
int64_t merge_align_sharpness(const int32_t *grey);
// HDR: marks the tiles where the reference has enough unclipped positions for camera B's offset to mean
// anything.
void merge_align_hdr_info(const uint16_t *reference, uint8_t *informative);
// HDR: replaces camera B's offsets on uninformative tiles. shakeY, shakeX: per tile, the matching camera A
// frame's offsets. scratch: TILE_COUNT x 2 int16.
void merge_align_hdr_fill(int16_t (*offsets)[MERGE_TILE_COLUMNS][AXIS_COUNT], const uint8_t *informative,
                          const int16_t *shakeY, const int16_t *shakeX, int16_t *scratch);
