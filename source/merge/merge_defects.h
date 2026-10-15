// Prepares a frame for the merge: the camera's stream unpacked, the mapped defects fixed.
#pragma once
#include <stdint.h>

// Unpacks a RAW10 frame to u16 in place. Each pixel's bytes are read before they are overwritten.
void merge_defects_unpack(uint8_t *frame);
// Replaces the mapped defects of one frame (x, y pairs) by the median of their same-colour neighbours. All
// medians are taken before any is written, so a defect never reads a neighbouring defect's new value. Returns
// the number of pixels changed.
int merge_defects_fix(uint16_t *frame, const uint16_t *defects, int defectCount);
