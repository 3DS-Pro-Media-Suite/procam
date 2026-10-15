// PNG, JPEG and raw files, and frames reduced, flipped and converted to RGB for saving.
#pragma once
#include <3ds.h>
#include "photo_metadata.h"

// rgb is 8-bit RGB, top row first. meta may be NULL.
bool image_files_save_png(const char *path, const u8 *rgb, int width, int height, const PhotoMeta *meta);
// Without chroma subsampling, with the same EXIF block as the PNGs.
bool image_files_save_jpeg(const char *path, const u8 *rgb, int width, int height, const PhotoMeta *meta);
// Shrinks by a whole factor in place, keeping the pixel at the centre of each block.
void image_files_reduce_rgb(u8 *rgb, int width, int height, int sizeDivisor, int *outWidth, int *outHeight);
// Mirrors in place by a CAMU flip value: horizontal, vertical or both.
void image_files_flip_rgb(u8 *rgb, int width, int height, int flip);
// A whole camera frame (format as in raw_view_decode_row) as 8-bit RGB, flipped, then reduced like
// image_files_reduce_rgb. malloc'd.
u8 *image_files_frame_rgb(const void *frame, int format, int sizeDivisor, int flip, int *outWidth,
                          int *outHeight);
bool image_files_save_frame_png(const char *path, const void *frame, int format, int sizeDivisor, int flip,
                                const PhotoMeta *meta);
bool image_files_save_bytes(const char *path, const void *data, size_t size);
