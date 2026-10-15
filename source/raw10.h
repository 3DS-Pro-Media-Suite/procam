// The camera's raw samples: 10 bits per pixel on an RGGB mosaic, stored as two bytes per pixel, a byte with
// the 8 high bits and then a byte holding the 2 low bits. Also the colour layout shared by everything that
// develops them. Portable C.
#pragma once
#include <stdint.h>

#define RAW10_BYTES_PER_PIXEL 2
#define RAW10_LOW_BITS 2
#define RAW10_LOW_MASK ((1 << RAW10_LOW_BITS) - 1)
#define RAW10_WHITE 1023 // the largest 10-bit sample
// Black level of every channel and gain, estimated from a dim scene.
#define RAW10_BLACK 38

// The cells of a 2 x 2 RGGB quad: R, Gr, Gb, B.
#define CFA_CELLS 4

// Colour channels, in this order everywhere.
enum {
	RGB_RED,
	RGB_GREEN,
	RGB_BLUE,
	RGB_CHANNELS
};

// A 3 x 3 colour matrix, row-major.
#define COLOR_MATRIX_ENTRIES (RGB_CHANNELS * RGB_CHANNELS)

// The 10-bit sample of one stored pixel.
static inline int raw10_unpack(const uint8_t *pixel)
{
	return (pixel[0] << RAW10_LOW_BITS) | (pixel[1] & RAW10_LOW_MASK);
}

// Stores a 10-bit sample.
static inline void raw10_pack(uint8_t *pixel, int value)
{
	pixel[0] = (uint8_t)(value >> RAW10_LOW_BITS);
	pixel[1] = (uint8_t)(value & RAW10_LOW_MASK);
}
