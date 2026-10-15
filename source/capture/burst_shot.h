// MERGE, HDR and 3D shots from the dual stream (or the slow fallback), handed to the background merge.
#pragma once
#include <3ds.h>
#include "settings.h"

// 3D: one frame from each outer camera at the same moment, saved as a pair in a folder.
void burst_shot_3d(const ShotSettings *shot);
// MERGE and HDR: a burst queued for the background merge.
void burst_shot_merge(const ShotSettings *shot, bool hdr);
