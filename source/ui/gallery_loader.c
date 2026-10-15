#include "gallery_loader.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <png.h>
#include <turbojpeg.h>
#include "app_threads.h"
#include "cam_settings.h"
#include "dng_file.h"
#include "gallery.h"
#include "preview.h"
#include "tiff_writer.h"
#include "raw_view.h"

#define MAX_FILE_SIZE (64 * 1024 * 1024)
#define READ_CHUNK_SIZE (256 * 1024)
#define MAX_PICTURE_SIDE 8192

// Larger pictures are reduced by a whole factor to fit the view.
#define SHOWN_MAX_WIDTH RAW_VIEW_MAX_WIDTH
#define SHOWN_MAX_HEIGHT CAM_FRAME_HEIGHT

#define RGB_BYTES_PER_PIXEL 3

// tjDecompress2: rows packed tightly, no decoding flags.
#define JPEG_PACKED_ROWS 0
#define JPEG_NO_FLAGS 0

#define JPEG_MIN_SIZE 4 // the start of image marker and the next marker
#define JPEG_MARKER 0xFF
#define JPEG_START_OF_IMAGE 0xD8
#define TIFF_MAGIC 42
#define PNG_SIGNATURE_SIZE 8
#define PNG_SIGNATURE_FIRST_BYTE 0x89

#define LOAD_THREAD_STACK_SIZE (16 * 1024)
#define LOAD_THREAD_PRIORITY_BELOW_MAIN 1 // a larger number is a lower priority

// A picture is read and decoded on a worker thread, so the D-pad stays responsive.
typedef struct {
	Thread thread;
	_Atomic bool busy;
	_Atomic bool done;
	bool succeeded;
	char path[GALLERY_PATH_MAX];
	char rightPath[GALLERY_PATH_MAX]; // the right eye of a 3D view, or empty
	int width;
	int height;
	int sourceWidth;
	int sourceHeight;
	int rightWidth;
	int rightHeight;
	u32 *pixels; // width x height RGBA8, top row first
	u32 *rightPixels;
} GalleryLoad;

static GalleryLoad g_galleryLoad;
static bool g_loaderBusy; // gallery_loader_poll's last answer

int g_gallerySourceWidth;
int g_gallerySourceHeight;

static bool read_in_chunks(FILE *file, u8 *buffer, size_t size)
{
	size_t bytesRead = 0;
	while (bytesRead < size) {
		size_t chunk = size - bytesRead;
		if (chunk > READ_CHUNK_SIZE) {
			chunk = READ_CHUNK_SIZE;
		}
		if (fread(buffer + bytesRead, 1, chunk, file) != chunk) {
			return false;
		}
		bytesRead += chunk;
	}
	return true;
}

// The whole file, malloc'd. Large reads are what the SD card is fast at.
static u8 *read_whole_file(const char *path, size_t *length)
{
	FILE *file = fopen(path, "rb");
	if (!file) {
		return NULL;
	}
	u8 *buffer = NULL;
	long size = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
	bool sizeOk = size > 0 && size < MAX_FILE_SIZE;
	if (sizeOk && fseek(file, 0, SEEK_SET) == 0) {
		buffer = malloc(size);
	}
	if (buffer && !read_in_chunks(file, buffer, (size_t)size)) {
		free(buffer);
		buffer = NULL;
	}
	fclose(file);
	*length = size > 0 ? (size_t)size : 0;
	return buffer;
}

static bool picture_side_ok(u32 side)
{
	return side > 0 && side <= MAX_PICTURE_SIDE;
}

static bool is_jpeg(const u8 *file, size_t length)
{
	return length > JPEG_MIN_SIZE && file[0] == JPEG_MARKER && file[1] == JPEG_START_OF_IMAGE;
}

// A little-endian TIFF: Pro Cam's own DNG.
static bool is_dng(const u8 *file, size_t length)
{
	return length > TIFF_HEADER_SIZE && file[0] == 'I' && file[1] == 'I' && file[2] == TIFF_MAGIC;
}

static bool is_png(const u8 *file, size_t length)
{
	return length > PNG_SIGNATURE_SIZE && file[0] == PNG_SIGNATURE_FIRST_BYTE && file[1] == 'P';
}

static u8 *decode_jpeg(const u8 *file, size_t length, int *width, int *height)
{
	tjhandle decoder = tjInitDecompress();
	int jpegWidth = 0;
	int jpegHeight = 0;
	int subsampling = 0;
	int colorspace = 0;
	u8 *rgb = NULL;
	if (decoder &&
	    !tjDecompressHeader3(decoder, file, length, &jpegWidth, &jpegHeight, &subsampling, &colorspace) &&
	    picture_side_ok(jpegWidth) && picture_side_ok(jpegHeight)) {
		rgb = malloc((size_t)jpegWidth * jpegHeight * RGB_BYTES_PER_PIXEL);
	}
	if (rgb && tjDecompress2(decoder, file, length, rgb, jpegWidth, JPEG_PACKED_ROWS, jpegHeight, TJPF_RGB,
	                         JPEG_NO_FLAGS)) {
		free(rgb);
		rgb = NULL;
	}
	if (decoder) {
		tjDestroy(decoder);
	}
	*width = jpegWidth;
	*height = jpegHeight;
	return rgb;
}

static u8 *decode_png(const u8 *file, size_t length, int *width, int *height)
{
	png_image image;
	memset(&image, 0, sizeof image);
	image.version = PNG_IMAGE_VERSION;
	u8 *rgb = NULL;
	if (png_image_begin_read_from_memory(&image, file, length)) {
		image.format = PNG_FORMAT_RGB;
		rgb = malloc(PNG_IMAGE_SIZE(image));
		if (!rgb || !png_image_finish_read(&image, NULL, rgb, 0, NULL)) {
			free(rgb);
			rgb = NULL;
		}
		*width = image.width;
		*height = image.height;
	}
	png_image_free(&image);
	return rgb;
}

// A JPG, a DNG or a PNG, as top-first RGB, malloc'd. NULL for any other file.
static u8 *decode_rgb(const u8 *file, size_t length, int *width, int *height)
{
	if (is_jpeg(file, length)) {
		return decode_jpeg(file, length, width, height);
	}
	if (is_dng(file, length)) {
		return dng_file_decode_rgb(file, length, width, height);
	}
	if (is_png(file, length)) {
		return decode_png(file, length, width, height);
	}
	return NULL;
}

static u8 *load_rgb(const char *path, int *width, int *height)
{
	size_t length = 0;
	u8 *file = read_whole_file(path, &length);
	if (!file) {
		return NULL;
	}
	u8 *rgb = decode_rgb(file, length, width, height);
	free(file);
	return rgb;
}

// A picture as RGBA8, reduced to fit the view. Nearest neighbour, so every shown pixel is a real one.
static u32 *decode_to_fit(const char *path, int *fitWidth, int *fitHeight, int *sourceWidth,
                          int *sourceHeight)
{
	int width = 0;
	int height = 0;
	u8 *rgb = load_rgb(path, &width, &height);
	if (!rgb) {
		return NULL;
	}
	int factor = 1;
	while (width / factor > SHOWN_MAX_WIDTH || height / factor > SHOWN_MAX_HEIGHT) {
		factor++;
	}
	int shownWidth = width / factor;
	int shownHeight = height / factor;
	u32 *pixels = malloc((size_t)shownWidth * shownHeight * sizeof(u32));
	bool ok = pixels && shownWidth > 0 && shownHeight > 0;
	for (int y = 0; ok && y < shownHeight; y++) {
		int sourceY = y * factor + factor / 2;
		const u8 *sourceRow = rgb + (size_t)sourceY * width * RGB_BYTES_PER_PIXEL;
		u32 *destination = pixels + (size_t)y * shownWidth;
		for (int x = 0; x < shownWidth; x++) {
			int sourceX = x * factor + factor / 2;
			const u8 *pixel = sourceRow + (size_t)sourceX * RGB_BYTES_PER_PIXEL;
			destination[x] = RGBA(pixel[0], pixel[1], pixel[2]);
		}
	}
	free(rgb);
	if (!ok) {
		free(pixels);
		return NULL;
	}
	*fitWidth = shownWidth;
	*fitHeight = shownHeight;
	*sourceWidth = width;
	*sourceHeight = height;
	return pixels;
}

static void load_thread(void *arg)
{
	(void)arg;
	g_galleryLoad.pixels = decode_to_fit(g_galleryLoad.path, &g_galleryLoad.width, &g_galleryLoad.height,
	                                     &g_galleryLoad.sourceWidth, &g_galleryLoad.sourceHeight);
	bool wantsRightEye = g_galleryLoad.rightPath[0] != 0;
	u32 *rightPixels = NULL;
	if (wantsRightEye) {
		int rightSourceWidth;
		int rightSourceHeight;
		rightPixels = decode_to_fit(g_galleryLoad.rightPath, &g_galleryLoad.rightWidth,
		                            &g_galleryLoad.rightHeight, &rightSourceWidth, &rightSourceHeight);
	}
	g_galleryLoad.rightPixels = rightPixels;
	bool rightEyeOk = rightPixels && g_galleryLoad.rightWidth == g_galleryLoad.width &&
	                  g_galleryLoad.rightHeight == g_galleryLoad.height;
	g_galleryLoad.succeeded = g_galleryLoad.pixels && (!wantsRightEye || rightEyeOk);
	g_galleryLoad.done = true;
	g_galleryLoad.busy = false;
}

static void start_load(const char *path, const char *rightPath)
{
	s32 mainPriority = MAIN_THREAD_PRIORITY;
	svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);
	snprintf(g_galleryLoad.path, sizeof g_galleryLoad.path, "%s", path);
	snprintf(g_galleryLoad.rightPath, sizeof g_galleryLoad.rightPath, "%s", rightPath);
	g_galleryLoad.done = false;
	g_galleryLoad.busy = true;
	s32 priority = mainPriority + LOAD_THREAD_PRIORITY_BELOW_MAIN;
	g_galleryLoad.thread =
	        threadCreate(load_thread, NULL, LOAD_THREAD_STACK_SIZE, priority, THREAD_CORE_APP, false);
	if (!g_galleryLoad.thread) {
		load_thread(NULL); // no thread to spare: load here
	}
}

static void copy_to_staging(const u32 *pixels, int width, int height)
{
	for (int y = 0; y < height; y++) {
		memcpy(preview_staging_row(y), pixels + (size_t)y * width, width * sizeof(u32));
	}
}

static void show_loaded_picture(void)
{
	preview_idle();
	preview_ensure_texture(g_galleryLoad.width, g_galleryLoad.height);
	// The right eye goes first, so the staging buffer and the view texture end with the main picture.
	if (g_galleryLoad.rightPixels) {
		copy_to_staging(g_galleryLoad.rightPixels, g_galleryLoad.width, g_galleryLoad.height);
		preview_upload_right_eye(g_galleryLoad.width, g_galleryLoad.height);
	}
	copy_to_staging(g_galleryLoad.pixels, g_galleryLoad.width, g_galleryLoad.height);
	preview_upload(g_galleryLoad.width, g_galleryLoad.height);
	gallery_set_stereo_shown(g_galleryLoad.rightPixels != NULL);
	g_gallerySourceWidth = g_galleryLoad.sourceWidth;
	g_gallerySourceHeight = g_galleryLoad.sourceHeight;
}

static void finish_load(void)
{
	g_galleryLoad.done = false;
	const GalleryState *gallery = gallery_state();
	bool samePicture = !strcmp(g_galleryLoad.path, gallery->path) &&
	                   !strcmp(g_galleryLoad.rightPath, gallery->rightPath);
	bool stillWanted = gallery->open && samePicture;
	if (stillWanted && g_galleryLoad.succeeded) {
		show_loaded_picture();
	}
	if (stillWanted) {
		gallery_set_load_failed(!g_galleryLoad.succeeded);
	}
	free(g_galleryLoad.pixels);
	free(g_galleryLoad.rightPixels);
	g_galleryLoad.pixels = NULL;
	g_galleryLoad.rightPixels = NULL;
}

static bool poll_load(void)
{
	if (g_galleryLoad.busy) {
		return true;
	}
	if (g_galleryLoad.thread) {
		threadJoin(g_galleryLoad.thread, U64_MAX);
		threadFree(g_galleryLoad.thread);
		g_galleryLoad.thread = NULL;
	}
	if (g_galleryLoad.done) {
		finish_load();
	}
	if (!gallery_take_load_request()) {
		return false;
	}
	start_load(gallery_state()->path, gallery_state()->rightPath);
	return true;
}

bool gallery_loader_poll(void)
{
	g_loaderBusy = poll_load();
	return g_loaderBusy;
}

bool gallery_loader_busy(void)
{
	return g_loaderBusy;
}
