// Merges one tile of the alt frames into the reference: the plain merge and the HDR merge.
#pragma once
#include "merge_common.h"

// One tile of a plain merge: the per-plane weighted sums of the reference and the alt frames, in
// scratch->weightedSum and scratch->weightSum.
void merge_tiles_plain(MergeContext *context, TileScratch *scratch, int tileY, int tileX);

// One tile of an HDR merge: camera A's estimate blended with camera B's, as quotients in scratch->weightedSum
// with scratch->weightSum set to 1.
void merge_tiles_hdr(MergeContext *context, TileScratch *scratch, HdrScratch *hdr, int tileY, int tileX);
