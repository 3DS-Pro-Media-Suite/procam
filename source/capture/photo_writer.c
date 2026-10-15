#include "photo_writer.h"
#include <3ds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cam_settings.h"
#include "dng_file.h"
#include "hot_pixel_learner.h"
#include "image_files.h"
#include "look.h"
#include "look_render.h"
#include "math_util.h"
#include "merge.h"
#include "photo_metadata.h"
#include "sensor_color_matrix.h"
#include "settings.h"
#include "shot_info.h"

#define RGB_PIXEL_BYTES RGB_CHANNELS // 8-bit RGB
#define EXTENSION_LENGTH 4 // ".PNG", ".DNG", ".JPG", ".TXT"
#define HOT_MAP_VALUES_PER_PIXEL 2 // x, y
#define HOT_PIXEL_THRESHOLD 48 // a pixel this far outside its neighbours is fixed (10-bit levels)
#define TITLE_SIZE 40
#define SIDECAR_LINE_SIZE 96
#define PHOTO_SIZE_LINE_SIZE 128
#define LOOK_RECIPE_SIZE 1024

// The camera's learned hot pixel map. The learner rebuilds its map now and then, so the saver works on a
// copy. Saver thread only: there is one copy.
static int copy_hot_pixel_map(int camera, const u16 **map)
{
	static u16 mapCopy[HOT_PIXEL_MAP_MAX * HOT_MAP_VALUES_PER_PIXEL];
	const u16 *learnedMap;
	int count = hot_pixel_learner_map(camera, &learnedMap);
	if (count > 0) {
		memcpy(mapCopy, learnedMap, (size_t)count * HOT_MAP_VALUES_PER_PIXEL * sizeof mapCopy[0]);
	}
	*map = mapCopy;
	return count > 0 ? count : 0;
}

// Nothing is written when text is NULL.
static void write_text_file(const char *path, const char *text)
{
	FILE *file = text ? fopen(path, "w") : NULL;
	if (!file) {
		return;
	}
	fputs(text, file);
	fclose(file);
}

// Out of memory, the text is left unchanged.
static void append_text(char **text, const char *line)
{
	if (!*text) {
		return;
	}
	char *longerText = realloc(*text, strlen(*text) + strlen(line) + 1);
	if (longerText) {
		*text = longerText;
		strcat(longerText, line);
	}
}

static int mean_black(const float blackLevels[CFA_CELLS])
{
	float sum = blackLevels[0] + blackLevels[1] + blackLevels[2] + blackLevels[3];
	return (int)(sum / CFA_CELLS + 0.5f);
}

// Mirrors and reduces a rendered photo as the job asks, then writes it as a JPG.
static bool save_rgb_as_jpg(const SaveJob *job, u8 *rgb, int width, int height, const char *path,
                            const PhotoMeta *meta)
{
	if (job->camSettings.flip) {
		image_files_flip_rgb(rgb, width, height, job->camSettings.flip);
	}
	int outputWidth = width;
	int outputHeight = height;
	if (job->sizeDivisor > 1) {
		image_files_reduce_rgb(rgb, width, height, job->sizeDivisor, &outputWidth, &outputHeight);
	}
	return image_files_save_jpeg(path, rgb, outputWidth, outputHeight, meta);
}

// The in-camera JPG of a raw frame: the look's render with the white balance of the shot.
static bool save_look_jpg(const SaveJob *job, const u8 *raw, int width, int height, const char *path,
                          const PhotoMeta *meta)
{
	u8 *rgb = malloc((size_t)width * height * RGB_PIXEL_BYTES);
	if (!rgb) {
		return false;
	}
	int black = mean_black(job->blackLevels);
	// A failed render leaves the buffer undefined, so there is no JPG then.
	if (!look_render(raw, width, height, black, RAW10_WHITE, job->whiteBalance, job->lookIndex, rgb)) {
		free(rgb);
		return false;
	}
	bool ok = save_rgb_as_jpg(job, rgb, width, height, path, meta);
	free(rgb);
	return ok;
}

// The JPG of a merged mosaic. An HDR merge's extra range is rolled off, not clipped.
static bool save_merged_jpg(const SaveJob *job, const u16 *mosaic, const float blackLevels[CFA_CELLS],
                            int white, float range, const PhotoMeta *meta)
{
	u8 *rgb = malloc((size_t)job->width * job->height * RGB_PIXEL_BYTES);
	bool rendered = rgb && look_render_hdr(mosaic, job->width, job->height, mean_black(blackLevels), white,
	                                       range, job->whiteBalance, job->lookIndex, rgb);
	bool ok = rendered && save_rgb_as_jpg(job, rgb, job->width, job->height, job->path, meta);
	free(rgb);
	return ok;
}

// A path in the shot's folder.
static void folder_file_path(const SaveJob *job, const char *fileName, char *path, size_t size)
{
	snprintf(path, size, "%s/%s", job->path, fileName);
}

// One eye of a raw 3D pair, always in full size: base.DNG, and base.JPG unless the format is RAW only.
// Adds the eye's fixed hot pixels to hotPixelsFixed.
static bool save_raw_eye(const SaveJob *job, u8 *raw, int camera, const char *basePath, PhotoMeta *meta,
                         int *hotPixelsFixed)
{
	if (job->hotFix) {
		const u16 *hotMap;
		int hotMapCount = copy_hot_pixel_map(camera, &hotMap);
		*hotPixelsFixed += look_render_fix_hot_pixels(raw, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, 0, 0, hotMap,
		                                              hotMapCount, HOT_PIXEL_THRESHOLD);
	}
	meta->camera = CAM_CAMERA_NAMES[camera];
	char filePath[SHOT_FILE_PATH_MAX];
	snprintf(filePath, sizeof filePath, "%s.DNG", basePath);
	bool ok = dng_file_save_raw(filePath, raw, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, job->camSettings.flip,
	                            job->whiteBalance, job->blackLevels, meta, sensor_color_matrix());
	if (job->photoFormat != PHOTO_FORMAT_RAW_ONLY) {
		snprintf(filePath, sizeof filePath, "%s.JPG", basePath);
		ok = save_look_jpg(job, raw, CAM_FRAME_WIDTH, CAM_FRAME_HEIGHT, filePath, meta) && ok;
	}
	return ok;
}

// A 3D pair's INFO.TXT: one frame per camera, stereo, not HDR.
static void write_pair_info(const SaveJob *job, const char *sidecarText)
{
	const int framesPerCamera = 1;
	const bool stereo = true;
	const bool hdr = false;
	char *info = shot_info_text("3d", framesPerCamera, stereo, hdr, job->exposureBias, job->exposureBias,
	                            &job->camSettings, sidecarText);
	char infoPath[SHOT_FILE_PATH_MAX];
	folder_file_path(job, "INFO.TXT", infoPath, sizeof infoPath);
	write_text_file(infoPath, info);
	free(info);
}

// A 3D pair's folder, or the folder of a file below the photo folder (a bracket's files).
static void make_shot_folder(const SaveJob *job)
{
	if (strncmp(job->path, PHOTO_DIR "/", strlen(PHOTO_DIR "/"))) {
		return;
	}
	char folder[SHOT_PATH_MAX];
	snprintf(folder, sizeof folder, "%s", job->path);
	if (job->kind == SAVE_PAIR) {
		mkdir(folder, FOLDER_MODE);
		return;
	}
	char *lastSlash = strrchr(folder, '/');
	if (lastSlash && lastSlash - folder > (int)strlen(PHOTO_DIR)) {
		*lastSlash = 0;
		mkdir(folder, FOLDER_MODE);
	}
}

// The path below the photo folder, without the extension: "IMG_0042", "IMG_0042/EV+1".
static void make_title(const SaveJob *job, char *title, size_t size)
{
	const char *relativePath = job->path + strlen(PHOTO_DIR "/");
	int length = (int)strlen(relativePath);
	if (job->kind != SAVE_PAIR) {
		const char *dot = strrchr(relativePath, '.');
		if (dot) {
			length = (int)(dot - relativePath);
		}
	}
	snprintf(title, size, "%.*s", length, relativePath);
}

static void append_photo_size(char **text, const SaveJob *job)
{
	int fullWidth = CAM_FRAME_WIDTH;
	int fullHeight = CAM_FRAME_HEIGHT;
	if (job->kind == SAVE_FRAME) {
		fullWidth = job->width;
		fullHeight = job->height;
	}
	int divisor = job->sizeDivisor;
	char line[PHOTO_SIZE_LINE_SIZE];
	snprintf(line, sizeof line,
	         "photo size: %dx%d, nearest neighbour (every %dx%d block's centre camera pixel)\n",
	         fullWidth / divisor, fullHeight / divisor, divisor, divisor);
	append_text(text, line);
}

static const char *exposure_mode_name(int exposureMode)
{
	if (exposureMode == CAM_EXPOSURE_MANUAL) {
		return "manual";
	}
	if (exposureMode == CAM_EXPOSURE_SHUTTER_PRIORITY) {
		return "shutter priority";
	}
	return "auto";
}

// The exposure the sensor really used.
static void append_exposure(char **text, const SaveJob *job)
{
	int exposureMode = job->camSettings.exposureMode;
	const char *isoMode = exposureMode == CAM_EXPOSURE_MANUAL ? "manual" : "auto";
	char line[SIDECAR_LINE_SIZE];
	snprintf(line, sizeof line, "exposure: %.6f s (%s), ISO %d (%s)\n", job->exposureSeconds,
	         exposure_mode_name(exposureMode), job->iso, isoMode);
	append_text(text, line);
}

// The sidecar text: the settings, and the processing behind the photo.
static char *make_sidecar_text(const SaveJob *job)
{
	bool raw = job->format == CAM_FORMAT_RAW;
	// RAW only has no JPG, so no photo size and no look.
	bool hasRgbPhoto = !raw || job->photoFormat != PHOTO_FORMAT_RAW_ONLY;
	char *text = photo_metadata_sidecar_text(&job->camSettings, job->exposureBias, job->note,
	                                         raw ? job->whiteBalance : NULL);
	if (!text) {
		return NULL;
	}
	if (hasRgbPhoto && job->sizeDivisor > 1) {
		append_photo_size(&text, job);
	}
	if (raw && hasRgbPhoto) {
		// The look's recipe, so the shot can be rendered again.
		char recipe[LOOK_RECIPE_SIZE];
		look_describe(job->lookIndex, recipe, sizeof recipe);
		append_text(&text, recipe);
	}
	if (job->exposureSeconds > 0) {
		append_exposure(&text, job);
	}
	return text;
}

// A merged photo (MERGE, HDR): flat files, no folder. The merge already fixed its hot pixels. An HDR merge's
// white lies above the metered exposure's: the DNG says how far as BaselineExposure.
static bool write_merged(SaveJob *job, PhotoMeta *meta, char **text)
{
	size_t extensionStart = strlen(job->path) - EXTENSION_LENGTH;
	char *extension = job->path + extensionStart;
	const u16 *mosaic = (const u16 *)(const void *)job->frameA;
	int white = job->white > 0 ? job->white : MERGE_SCALE * RAW10_WHITE;
	float range = job->range > 1 ? job->range : 1;
	float blackLevels[CFA_CELLS];
	for (int cell = 0; cell < CFA_CELLS; cell++) {
		blackLevels[cell] = job->blackLevels[cell] * MERGE_SCALE;
	}
	bool ok = true;
	int cameraIndex = math_clamp_int(job->camera, 0, CAM_COUNT - 1);
	meta->camera = CAM_CAMERA_NAMES[cameraIndex];
	if (job->photoFormat != PHOTO_FORMAT_JPG_ONLY) {
		strcpy(extension, ".DNG");
		float baselineExposure = range > 1 ? log2f(range) : 0;
		ok = dng_file_save_mosaic(job->path, mosaic, job->width, job->height, white, job->camSettings.flip,
		                          job->whiteBalance, blackLevels, meta, sensor_color_matrix(),
		                          baselineExposure);
	}
	if (job->photoFormat != PHOTO_FORMAT_RAW_ONLY) {
		strcpy(extension, ".JPG");
		ok = save_merged_jpg(job, mosaic, blackLevels, white, range, meta) && ok;
	}
	// The merge log goes into the TXT only.
	if (job->mergeLog) {
		append_text(text, job->mergeLog);
	}
	strcpy(extension, ".TXT");
	write_text_file(job->path, *text);
	return ok;
}

// A raw frame: the DNG is the sensor data as streamed, the JPG the in-camera render.
static bool write_raw_frame(SaveJob *job, PhotoMeta *meta, char **text)
{
	size_t extensionStart = strlen(job->path) - EXTENSION_LENGTH;
	char *extension = job->path + extensionStart;
	bool ok = true;
	// The fix goes into the raw itself, so the DNG and the JPG both get it.
	if (job->hotFix) {
		const u16 *hotMap;
		int hotMapCount = copy_hot_pixel_map(job->camSettings.camera, &hotMap);
		int fixedCount = look_render_fix_hot_pixels(job->frameA, job->width, job->height, 0, 0, hotMap,
		                                            hotMapCount, HOT_PIXEL_THRESHOLD);
		if (*text) {
			char line[SIDECAR_LINE_SIZE];
			snprintf(line, sizeof line,
			         "hot pixels fixed in the raw: %d (%d mapped, the rest by neighbour check)\n", fixedCount,
			         hotMapCount);
			append_text(text, line);
			meta->text = *text;
		}
	}
	if (job->photoFormat != PHOTO_FORMAT_JPG_ONLY) {
		strcpy(extension, ".DNG");
		ok = dng_file_save_raw(job->path, job->frameA, job->width, job->height, job->camSettings.flip,
		                       job->whiteBalance, job->blackLevels, meta, sensor_color_matrix());
	}
	if (job->photoFormat != PHOTO_FORMAT_RAW_ONLY) {
		strcpy(extension, ".JPG");
		ok = save_look_jpg(job, job->frameA, job->width, job->height, job->path, meta) && ok;
	}
	strcpy(extension, ".TXT");
	write_text_file(job->path, *text);
	return ok;
}

// A YUYV or RGB565 frame. A YUYV frame also keeps the camera's own bytes: Y0 U Y1 V per pixel pair.
static bool write_frame(SaveJob *job, const PhotoMeta *meta, const char *text)
{
	size_t extensionStart = strlen(job->path) - EXTENSION_LENGTH;
	char *extension = job->path + extensionStart;
	bool ok = image_files_save_frame_png(job->path, job->frameA, job->format, job->sizeDivisor, FLIP_NONE,
	                                     meta);
	strcpy(extension, ".YUV");
	if (ok && job->format != CAM_FORMAT_RGB565) {
		ok = image_files_save_bytes(job->path, job->frameA, CAM_FRAME_BYTES);
	}
	strcpy(extension, ".TXT");
	write_text_file(job->path, text);
	return ok;
}

// A raw 3D pair: each eye in full as a DNG, and its in-camera JPG.
static bool write_raw_pair(SaveJob *job, PhotoMeta *meta, char **text)
{
	int hotPixelsFixed = 0;
	char basePath[SHOT_FILE_PATH_MAX];
	folder_file_path(job, "L", basePath, sizeof basePath);
	bool ok = save_raw_eye(job, job->frameA, CAM_OUTER_LEFT, basePath, meta, &hotPixelsFixed);
	folder_file_path(job, "R", basePath, sizeof basePath);
	ok = save_raw_eye(job, job->frameB, CAM_OUTER_RIGHT, basePath, meta, &hotPixelsFixed) && ok;
	if (job->hotFix) {
		char line[SIDECAR_LINE_SIZE];
		snprintf(line, sizeof line, "hot pixels fixed in the raw: %d (both eyes)\n", hotPixelsFixed);
		append_text(text, line);
	}
	write_pair_info(job, *text);
	return ok;
}

// Both eyes in one picture, the left eye on the left. NULL when an eye or the memory is missing.
static u8 *join_side_by_side(const u8 *left, const u8 *right, int width, int height)
{
	if (!left || !right) {
		return NULL;
	}
	size_t eyeRowBytes = (size_t)width * RGB_PIXEL_BYTES;
	size_t pairRowBytes = eyeRowBytes * 2;
	u8 *joined = malloc(pairRowBytes * height);
	if (!joined) {
		return NULL;
	}
	for (int y = 0; y < height; y++) {
		u8 *joinedRow = joined + y * pairRowBytes;
		memcpy(joinedRow, left + y * eyeRowBytes, eyeRowBytes);
		memcpy(joinedRow + eyeRowBytes, right + y * eyeRowBytes, eyeRowBytes);
	}
	return joined;
}

// A YUYV 3D pair: both eyes side by side for parallel viewing, then each eye on its own.
static bool write_pair(SaveJob *job, PhotoMeta *meta, const char *text)
{
	int divisor = job->sizeDivisor;
	int width;
	int height;
	u8 *left = image_files_frame_rgb(job->frameA, CAM_FORMAT_YUYV, divisor, FLIP_NONE, &width, &height);
	u8 *right = image_files_frame_rgb(job->frameB, CAM_FORMAT_YUYV, divisor, FLIP_NONE, &width, &height);
	u8 *sideBySide = join_side_by_side(left, right, width, height);
	bool ok = sideBySide != NULL;
	char filePath[SHOT_FILE_PATH_MAX];

	meta->camera = "Outer cameras (stereo pair: left half = left eye OUT2, right half = right eye OUT1)";
	folder_file_path(job, "SBS.PNG", filePath, sizeof filePath);
	ok = ok && image_files_save_png(filePath, sideBySide, width * 2, height, meta);
	free(sideBySide);

	meta->camera = CAM_CAMERA_NAMES[CAM_OUTER_LEFT];
	folder_file_path(job, "L.PNG", filePath, sizeof filePath);
	ok = ok && image_files_save_png(filePath, left, width, height, meta);

	meta->camera = CAM_CAMERA_NAMES[CAM_OUTER_RIGHT];
	folder_file_path(job, "R.PNG", filePath, sizeof filePath);
	ok = ok && image_files_save_png(filePath, right, width, height, meta);
	free(left);
	free(right);

	folder_file_path(job, "L.YUV", filePath, sizeof filePath);
	ok = ok && image_files_save_bytes(filePath, job->frameA, CAM_FRAME_BYTES);
	folder_file_path(job, "R.YUV", filePath, sizeof filePath);
	ok = ok && image_files_save_bytes(filePath, job->frameB, CAM_FRAME_BYTES);

	write_pair_info(job, text);
	return ok;
}

bool photo_writer_write(SaveJob *job)
{
	make_shot_folder(job);
	if (job->kind == SAVE_TEXT) {
		write_text_file(job->path, job->note);
		return job->note != NULL;
	}
	char title[TITLE_SIZE];
	make_title(job, title, sizeof title);
	bool raw = job->format == CAM_FORMAT_RAW;
	char *text = make_sidecar_text(job);
	PhotoMeta meta = {
	        .title = title,
	        .camera = CAM_CAMERA_NAMES[(int)job->camSettings.camera],
	        .text = text,
	        .camSettings = &job->camSettings,
	        .exposureBias = job->exposureBias,
	        .when = job->when,
	        .exposureSeconds = job->exposureSeconds,
	        .iso = job->iso,
	        .raw = raw,
	};
	bool ok = false;
	if (job->kind == SAVE_MERGED) {
		ok = write_merged(job, &meta, &text);
	} else if (job->kind == SAVE_FRAME && raw) {
		ok = write_raw_frame(job, &meta, &text);
	} else if (job->kind == SAVE_FRAME) {
		ok = write_frame(job, &meta, text);
	} else if (job->kind == SAVE_PAIR && raw) {
		ok = write_raw_pair(job, &meta, &text);
	} else if (job->kind == SAVE_PAIR) {
		ok = write_pair(job, &meta, text);
	}
	free(text);
	return ok;
}
