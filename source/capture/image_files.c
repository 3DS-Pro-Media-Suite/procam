#include "image_files.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <png.h>
#include <turbojpeg.h>
#include "cam_settings.h"
#include "photo_metadata.h"
#include "raw_view.h"

#define RGB_BYTES 3 // bytes per pixel

// libpng writes in small pieces; a large stdio buffer turns them into few SD writes.
#define PNG_FILE_BUFFER_SIZE (256 * 1024)
// The 3DS deflates slowly, and photo noise barely compresses further at higher levels.
#define PNG_ZLIB_LEVEL 1
#define PNG_ZLIB_BUFFER_SIZE (128 * 1024) // fewer, larger IDAT chunks
#define PNG_BIT_DEPTH 8
#define PNG_TIME_TEXT_SIZE 40
#define PNG_TIME_FORMAT "%Y-%m-%dT%H:%M:%S"
#define PNG_SOFTWARE_TEXT "Pro Cam (Nintendo 3DS homebrew)"

#define JPEG_QUALITY 95
#define JPEG_PACKED_ROWS 0 // a pitch of 0 tells turbojpeg the rows have no padding
#define JPEG_MARKER_SIZE 2
#define JPEG_MARKER_PREFIX 0xFF
#define JPEG_APP1_MARKER 0xE1
#define EXIF_SEGMENT_HEADER_SIZE 10 // the APP1 marker, the segment length and "Exif\0\0"

static void set_png_metadata(png_structp png, png_infop info, const PhotoMeta *meta)
{
	char creationTime[PNG_TIME_TEXT_SIZE] = "";
	struct tm fields;
	if (gmtime_r(&meta->when, &fields)) {
		strftime(creationTime, sizeof creationTime, PNG_TIME_FORMAT, &fields);
	}
	const char *description = meta->text ? meta->text : "";
	png_text texts[] = {
	        {.compression = PNG_TEXT_COMPRESSION_NONE, .key = "Title", .text = (png_charp)meta->title},
	        {.compression = PNG_TEXT_COMPRESSION_NONE, .key = "Software", .text = PNG_SOFTWARE_TEXT},
	        {.compression = PNG_TEXT_COMPRESSION_NONE, .key = "Creation Time", .text = creationTime},
	        {.compression = PNG_TEXT_COMPRESSION_NONE, .key = "Source", .text = (png_charp)meta->camera},
	        {.compression = PNG_TEXT_COMPRESSION_zTXt, .key = "Description", .text = (png_charp)description},
	};
	png_set_text(png, info, texts, sizeof texts / sizeof texts[0]);
	u8 exif[PHOTO_METADATA_EXIF_MAX_SIZE];
	size_t exifSize = photo_metadata_make_exif(exif, meta);
	png_set_eXIf_1(png, info, (png_uint_32)exifSize, exif);
}

static void write_png(png_structp png, png_infop info, FILE *file, const u8 *rgb, int width, int height,
                      const PhotoMeta *meta)
{
	png_init_io(png, file);
	png_set_compression_level(png, PNG_ZLIB_LEVEL);
	png_set_compression_buffer_size(png, PNG_ZLIB_BUFFER_SIZE);
	png_set_filter(png, PNG_FILTER_TYPE_BASE, PNG_FILTER_SUB);
	png_set_IHDR(png, info, width, height, PNG_BIT_DEPTH, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
	             PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_set_sRGB(png, info, PNG_sRGB_INTENT_PERCEPTUAL); // camera pictures are sRGB-encoded
	if (meta) {
		set_png_metadata(png, info, meta);
	}
	png_write_info(png, info);
	for (int y = 0; y < height; y++) {
		png_write_row(png, rgb + (size_t)y * width * RGB_BYTES);
	}
	png_write_end(png, NULL);
}

bool image_files_save_png(const char *path, const u8 *rgb, int width, int height, const PhotoMeta *meta)
{
	FILE *file = fopen(path, "wb");
	if (!file) {
		return false;
	}
	char *fileBuffer = malloc(PNG_FILE_BUFFER_SIZE);
	if (fileBuffer) {
		setvbuf(file, fileBuffer, _IOFBF, PNG_FILE_BUFFER_SIZE);
	}
	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	volatile bool written = false;
	// libpng reports errors by jumping back here.
	if (info && !setjmp(png_jmpbuf(png))) {
		write_png(png, info, file, rgb, width, height, meta);
		written = true;
	}
	if (png) {
		png_destroy_write_struct(&png, info ? &info : NULL);
	}
	bool closed = fclose(file) == 0; // fclose flushes the buffer, so it is freed only afterwards
	free(fileBuffer);
	return closed && written;
}

static bool write_exif_segment(FILE *file, const u8 *exif, size_t exifSize)
{
	// The segment length counts itself, the signature and the EXIF block, not the marker.
	size_t segmentLength = exifSize + EXIF_SEGMENT_HEADER_SIZE - JPEG_MARKER_SIZE;
	u8 lengthHigh = (u8)(segmentLength >> 8); // JPEG numbers are big-endian
	u8 lengthLow = (u8)segmentLength;
	u8 header[EXIF_SEGMENT_HEADER_SIZE] = {
	        JPEG_MARKER_PREFIX, JPEG_APP1_MARKER, lengthHigh, lengthLow, 'E', 'x', 'i', 'f', 0, 0};
	if (fwrite(header, 1, sizeof header, file) != sizeof header) {
		return false;
	}
	return fwrite(exif, 1, exifSize, file) == exifSize;
}

// The EXIF block goes in an APP1 segment right after the start-of-image marker.
static bool write_jpeg_with_exif(FILE *file, const u8 *jpeg, unsigned long jpegSize, const u8 *exif,
                                 size_t exifSize)
{
	if (fwrite(jpeg, 1, JPEG_MARKER_SIZE, file) != JPEG_MARKER_SIZE) {
		return false;
	}
	if (exifSize > 0 && !write_exif_segment(file, exif, exifSize)) {
		return false;
	}
	size_t restSize = jpegSize - JPEG_MARKER_SIZE;
	return fwrite(jpeg + JPEG_MARKER_SIZE, 1, restSize, file) == restSize;
}

bool image_files_save_jpeg(const char *path, const u8 *rgb, int width, int height, const PhotoMeta *meta)
{
	tjhandle compressor = tjInitCompress();
	if (!compressor) {
		return false;
	}
	unsigned char *jpeg = NULL;
	unsigned long jpegSize = 0;
	int error = tjCompress2(compressor, rgb, width, JPEG_PACKED_ROWS, height, TJPF_RGB, &jpeg, &jpegSize,
	                        TJSAMP_444, JPEG_QUALITY, 0);
	tjDestroy(compressor);
	// A JPEG has at least its start and end markers.
	if (error || jpegSize < 2 * JPEG_MARKER_SIZE) {
		tjFree(jpeg);
		return false;
	}
	u8 exif[PHOTO_METADATA_EXIF_MAX_SIZE];
	size_t exifSize = meta ? photo_metadata_make_exif(exif, meta) : 0;
	FILE *file = fopen(path, "wb");
	if (!file) {
		tjFree(jpeg);
		return false;
	}
	bool written = write_jpeg_with_exif(file, jpeg, jpegSize, exif, exifSize);
	tjFree(jpeg);
	bool closed = fclose(file) == 0;
	return closed && written;
}

void image_files_reduce_rgb(u8 *rgb, int width, int height, int sizeDivisor, int *outWidth, int *outHeight)
{
	*outWidth = width / sizeDivisor;
	*outHeight = height / sizeDivisor;
	int centre = sizeDivisor / 2;
	for (int y = 0; y < *outHeight; y++) {
		for (int x = 0; x < *outWidth; x++) {
			size_t source = (size_t)(y * sizeDivisor + centre) * width + x * sizeDivisor + centre;
			size_t destination = (size_t)y * *outWidth + x;
			memmove(rgb + destination * RGB_BYTES, rgb + source * RGB_BYTES, RGB_BYTES);
		}
	}
}

static void swap_pixels(u8 *first, u8 *second)
{
	u8 saved[RGB_BYTES];
	memcpy(saved, first, RGB_BYTES);
	memcpy(first, second, RGB_BYTES);
	memcpy(second, saved, RGB_BYTES);
}

static void mirror_horizontally(u8 *rgb, int width, int height)
{
	for (int y = 0; y < height; y++) {
		u8 *row = rgb + (size_t)y * width * RGB_BYTES;
		int left = 0;
		int right = width - 1;
		while (left < right) {
			swap_pixels(row + (size_t)left * RGB_BYTES, row + (size_t)right * RGB_BYTES);
			left++;
			right--;
		}
	}
}

static void mirror_vertically(u8 *rgb, int width, int height)
{
	size_t rowBytes = (size_t)width * RGB_BYTES;
	u8 *savedRow = malloc(rowBytes);
	if (!savedRow) {
		return;
	}
	int top = 0;
	int bottom = height - 1;
	while (top < bottom) {
		u8 *topRow = rgb + top * rowBytes;
		u8 *bottomRow = rgb + bottom * rowBytes;
		memcpy(savedRow, topRow, rowBytes);
		memcpy(topRow, bottomRow, rowBytes);
		memcpy(bottomRow, savedRow, rowBytes);
		top++;
		bottom--;
	}
	free(savedRow);
}

void image_files_flip_rgb(u8 *rgb, int width, int height, int flip)
{
	if (flip & FLIP_HORIZONTAL) {
		mirror_horizontally(rgb, width, height);
	}
	if (flip & FLIP_VERTICAL) {
		mirror_vertically(rgb, width, height);
	}
}

// No averaging: every output pixel is a real camera pixel, the one at the centre of its block.
static void copy_block_centres(const u8 *source, int sourceWidth, int sizeDivisor, u8 *destination, int width,
                               int height)
{
	int centre = sizeDivisor / 2;
	for (int y = 0; y < height; y++) {
		const u8 *sourceRow = source + (size_t)(y * sizeDivisor + centre) * sourceWidth * RGB_BYTES;
		for (int x = 0; x < width; x++) {
			u8 *pixel = destination + ((size_t)y * width + x) * RGB_BYTES;
			memcpy(pixel, sourceRow + (size_t)(x * sizeDivisor + centre) * RGB_BYTES, RGB_BYTES);
		}
	}
}

u8 *image_files_frame_rgb(const void *frame, int format, int sizeDivisor, int flip, int *outWidth,
                          int *outHeight)
{
	u8 *rgb = raw_view_frame_rgb(frame, format, CAM_FRAME_WIDTH, 0, 0, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT);
	if (rgb && flip) {
		image_files_flip_rgb(rgb, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, flip);
	}
	*outWidth = CAM_FRAME_WIDTH;
	*outHeight = CAM_FRAME_HEIGHT;
	if (!rgb || sizeDivisor <= 1) {
		return rgb;
	}
	int width = CAM_FRAME_WIDTH / sizeDivisor;
	int height = CAM_FRAME_HEIGHT / sizeDivisor;
	u8 *reduced = malloc((size_t)width * height * RGB_BYTES);
	if (reduced) {
		copy_block_centres(rgb, CAM_FRAME_WIDTH, sizeDivisor, reduced, width, height);
	}
	free(rgb);
	*outWidth = width;
	*outHeight = height;
	return reduced;
}

bool image_files_save_frame_png(const char *path, const void *frame, int format, int sizeDivisor, int flip,
                                const PhotoMeta *meta)
{
	int width;
	int height;
	u8 *rgb = image_files_frame_rgb(frame, format, sizeDivisor, flip, &width, &height);
	bool saved = rgb && image_files_save_png(path, rgb, width, height, meta);
	free(rgb);
	return saved;
}

bool image_files_save_bytes(const char *path, const void *data, size_t size)
{
	FILE *file = fopen(path, "wb");
	if (!file) {
		return false;
	}
	bool written = fwrite(data, 1, size, file) == size;
	bool closed = fclose(file) == 0;
	return closed && written;
}
