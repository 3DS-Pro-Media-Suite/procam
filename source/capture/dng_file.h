// Writes DNG files and reads Pro Cam's own DNG back for display.
#pragma once
#include <3ds.h>
#include "photo_metadata.h"

// Saves 16-bit RGGB samples. white: their white level. blackLevels: one per CFA cell, in the samples' units.
// flip: the CAMU flip value. calibration: white-balanced camera RGB to linear sRGB, or NULL. baselineEv: how
// many EV a raw converter should brighten the picture by (an HDR merge), 0 for none.
bool dng_file_save_mosaic(const char *path, const u16 *samples, int width, int height, int white, int flip,
                          const float *whiteBalance, const float *blackLevels, const PhotoMeta *meta,
                          const float *calibration, float baselineEv);
// Saves a frame as streamed: per pixel a byte with the 8 high bits, then a byte with the 2 low bits.
bool dng_file_save_raw(const char *path, const u8 *raw, int width, int height, int flip,
                       const float *whiteBalance, const float *blackLevels, const PhotoMeta *meta,
                       const float *calibration);
// Pro Cam's own DNG as 8-bit RGB through the Standard look, oriented as it says. NULL for other files.
u8 *dng_file_decode_rgb(const u8 *file, size_t length, int *outWidth, int *outHeight);
