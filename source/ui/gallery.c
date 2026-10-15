#include "gallery.h"
#include <3ds.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "math_util.h"
#include "shot_info.h"
#include "photo_writer.h"
#include "saver.h"
#include "settings.h"
#include "shot_names.h"

// Items are single photos (IMG_nnnn.JPG, .DNG or .PNG) and folders (IMG_nnnn/) that hold a bracket or a 3D
// pair. Up and Down step through the pictures of a folder.

#define EXTENSION_LENGTH 4 // ".PNG"
#define STEREO_ENTRY_NAME "3D" // the extra folder entry that shows a stereo pair in 3D
#define SLIDER_MAX_DEPTH 20.0f // pixels between the eyes with the 3D slider at the top
#define CONVERGENCE_MAX 60 // the most the eyes move apart or together, in screen pixels

// Display order inside a folder.
enum {
	RANK_OVERVIEW, // a 3D pair's side-by-side picture
	RANK_OTHER,
};

static GalleryState g_gallery;

// A merged photo is being saved: re-list once the saver is done.
static bool g_refreshPending;
static int g_refreshShotId;

static int compare_items(const void *a, const void *b)
{
	const GalleryItem *itemA = a;
	const GalleryItem *itemB = b;
	return itemA->id - itemB->id;
}

// IMG_nnnn is a multi-frame shot's folder, IMG_nnnn.ext a single photo. Other names are sidecars.
static bool parse_item_name(const char *name, GalleryItem *item)
{
	int number;
	int numberEnd = 0;
	if (sscanf(name, "IMG_%d%n", &number, &numberEnd) != 1) {
		return false;
	}
	if (number < 0 || number > SHOT_NUMBER_MAX) {
		return false;
	}
	const char *extension = name + numberEnd;
	item->id = number;
	if (!*extension) {
		item->folder = 1;
	} else if (!strcasecmp(extension, ".PNG")) {
		item->pictureType = GALLERY_PICTURE_PNG;
	} else if (!strcasecmp(extension, ".JPG")) {
		item->pictureType = GALLERY_PICTURE_JPG;
	} else if (!strcasecmp(extension, ".DNG")) {
		item->pictureType = GALLERY_PICTURE_DNG;
	} else {
		return false;
	}
	return true;
}

// Two pictures of one number (DNG + JPG) make one item. A DNG gives way to the other picture.
static void merge_duplicate_items(void)
{
	for (int i = 1; i < g_gallery.count; i++) {
		GalleryItem *previous = &g_gallery.items[i - 1];
		GalleryItem *current = &g_gallery.items[i];
		bool samePhoto = current->id == previous->id && !current->folder && !previous->folder;
		if (!samePhoto) {
			continue;
		}
		if (previous->pictureType != GALLERY_PICTURE_DNG && current->pictureType == GALLERY_PICTURE_DNG) {
			*current = *previous;
		}
		int itemsFromCurrent = g_gallery.count - i;
		memmove(previous, current, itemsFromCurrent * sizeof g_gallery.items[0]);
		g_gallery.count--;
		i--;
	}
}

void gallery_scan(void)
{
	g_gallery.count = 0;
	DIR *dir = opendir(PHOTO_DIR);
	if (!dir) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(dir)) && g_gallery.count < GALLERY_MAX_ITEMS) {
		GalleryItem item = {0};
		if (!parse_item_name(entry->d_name, &item)) {
			continue;
		}
		g_gallery.items[g_gallery.count] = item;
		g_gallery.count++;
	}
	closedir(dir);
	qsort(g_gallery.items, g_gallery.count, sizeof g_gallery.items[0], compare_items);
	merge_duplicate_items();
}

static const char *photo_extension(int pictureType)
{
	if (pictureType == GALLERY_PICTURE_DNG) {
		return "DNG";
	}
	if (pictureType == GALLERY_PICTURE_JPG) {
		return "JPG";
	}
	return "PNG";
}

static void item_path(const GalleryItem *item, char *path, size_t size)
{
	if (item->folder) {
		snprintf(path, size, PHOTO_DIR "/IMG_%04d", item->id);
		return;
	}
	snprintf(path, size, PHOTO_DIR "/IMG_%04d.%s", item->id, photo_extension(item->pictureType));
}

static bool has_prefix(const char *name, const char *prefix)
{
	return !strncasecmp(name, prefix, strlen(prefix));
}

static bool has_extension(const char *name, const char *extension)
{
	size_t length = strlen(name);
	return !strcasecmp(name + length - EXTENSION_LENGTH, extension);
}

static int file_rank(const char *name)
{
	return has_prefix(name, "SBS.") ? RANK_OVERVIEW : RANK_OTHER;
}

static int compare_files(const void *a, const void *b)
{
	const char *nameA = a;
	const char *nameB = b;
	int rankOrder = file_rank(nameA) - file_rank(nameB);
	if (rankOrder) {
		return rankOrder;
	}
	return strcasecmp(nameA, nameB);
}

static void list_pictures(const char *folder)
{
	DIR *dir = opendir(folder);
	if (!dir) {
		return;
	}
	struct dirent *entry;
	while ((entry = readdir(dir)) && g_gallery.fileCount < GALLERY_MAX_FILES) {
		const char *name = entry->d_name;
		size_t length = strlen(name);
		if (length <= EXTENSION_LENGTH || length >= GALLERY_FILE_NAME_SIZE) {
			continue;
		}
		if (!has_extension(name, ".PNG") && !has_extension(name, ".JPG") && !has_extension(name, ".DNG")) {
			continue;
		}
		strcpy(g_gallery.files[g_gallery.fileCount], name);
		g_gallery.fileCount++;
	}
	closedir(dir);
}

static bool jpg_listed_for(const char *dngName)
{
	size_t length = strlen(dngName);
	size_t baseLength = length - EXTENSION_LENGTH;
	for (int i = 0; i < g_gallery.fileCount; i++) {
		const char *name = g_gallery.files[i];
		if (strlen(name) == length && !strncasecmp(name, dngName, baseLength) &&
		    !strcasecmp(name + baseLength, ".JPG")) {
			return true;
		}
	}
	return false;
}

// A DNG with a JPG of the same frame next to it is not listed: the JPG is shown, as for single photos.
static void drop_dngs_with_jpg(void)
{
	for (int i = 0; i < g_gallery.fileCount; i++) {
		const char *name = g_gallery.files[i];
		if (!has_extension(name, ".DNG") || !jpg_listed_for(name)) {
			continue;
		}
		int filesAfter = g_gallery.fileCount - i - 1;
		memmove(g_gallery.files[i], g_gallery.files[i + 1], filesAfter * sizeof g_gallery.files[0]);
		g_gallery.fileCount--;
		i--;
	}
}

static bool file_exists_in(const char *folder, const char *name)
{
	char path[SHOT_FILE_PATH_MAX];
	struct stat status;
	snprintf(path, sizeof path, "%s/%s", folder, name);
	return stat(path, &status) == 0;
}

static void read_folder_info(const char *folder)
{
	char infoPath[SHOT_FILE_PATH_MAX];
	snprintf(infoPath, sizeof infoPath, "%s/INFO.TXT", folder);
	shot_info_read(infoPath, &g_gallery.info);
	if (!strcmp(g_gallery.info.kind, "3d")) {
		// A raw pair has L.DNG and R.DNG with their JPGs. A pair from the YUV stream has SBS, L and R PNGs.
		g_gallery.info.dng = file_exists_in(folder, "L.DNG");
	}
}

// A 3D shot's folder starts with the entry that shows the pair in 3D.
static void insert_stereo_entry(void)
{
	bool isStereoPair = !strcmp(g_gallery.info.kind, "3d");
	if (!isStereoPair || g_gallery.fileCount >= GALLERY_MAX_FILES) {
		return;
	}
	memmove(g_gallery.files[1], g_gallery.files[0], g_gallery.fileCount * sizeof g_gallery.files[0]);
	strcpy(g_gallery.files[0], STEREO_ENTRY_NAME);
	g_gallery.fileCount++;
}

static void list_folder(const char *folder)
{
	g_gallery.fileCount = 0;
	list_pictures(folder);
	drop_dngs_with_jpg();
	qsort(g_gallery.files, g_gallery.fileCount, sizeof g_gallery.files[0], compare_files);
	read_folder_info(folder);
	insert_stereo_entry();
}

// Each eye of a 3D shot is a JPG, a DNG when shot RAW only, or a PNG.
static const char *pair_extension(const char *folder, const char *leftName)
{
	static const char *const EXTENSIONS[] = {".JPG", ".DNG", ".PNG"};
	const char *extension = NULL;
	for (int i = 0; i < ARRAY_COUNT(EXTENSIONS); i++) {
		extension = EXTENSIONS[i];
		char path[GALLERY_PATH_MAX];
		struct stat status;
		snprintf(path, sizeof path, "%s/%s%s", folder, leftName, extension);
		if (stat(path, &status) == 0) {
			break;
		}
	}
	return extension;
}

// A 3D shot's L and R pictures.
static void select_stereo_pair(const char *folder)
{
	const char *leftName = "L";
	const char *rightName = "R";
	const char *extension = pair_extension(folder, leftName);
	snprintf(g_gallery.path, sizeof g_gallery.path, "%s/%s%s", folder, leftName, extension);
	snprintf(g_gallery.rightPath, sizeof g_gallery.rightPath, "%s/%s%s", folder, rightName, extension);
}

static void select_folder_picture(const char *folder, int file)
{
	list_folder(folder);
	bool fileListed = file >= 0 && file < g_gallery.fileCount;
	g_gallery.fileIndex = fileListed ? file : 0;
	g_gallery.rightPath[0] = 0;
	if (!g_gallery.fileCount) {
		g_gallery.path[0] = 0; // a folder still being written, or nothing to show
		return;
	}
	const char *name = g_gallery.files[g_gallery.fileIndex];
	if (!strcmp(name, STEREO_ENTRY_NAME)) {
		select_stereo_pair(folder);
		return;
	}
	snprintf(g_gallery.path, sizeof g_gallery.path, "%s/%s", folder, name);
}

void gallery_select(int index, int file)
{
	g_gallery.loadFailed = false;
	g_gallery.deleteArmedUntil = 0;
	if (!g_gallery.count) {
		g_gallery.path[0] = 0;
		return;
	}
	g_gallery.index = math_clamp_int(index, 0, g_gallery.count - 1);
	const GalleryItem *item = &g_gallery.items[g_gallery.index];
	char path[SHOT_PATH_MAX];
	item_path(item, path, sizeof path);
	memset(&g_gallery.info, 0, sizeof g_gallery.info);
	g_gallery.fileCount = 0;
	if (item->folder) {
		select_folder_picture(path, file);
	} else {
		snprintf(g_gallery.path, sizeof g_gallery.path, "%s", path);
		g_gallery.rightPath[0] = 0;
	}
	g_gallery.stereoShown = false; // until the new picture is loaded
	g_gallery.loading = g_gallery.path[0] != 0;
	g_gallery.loadFailed = !g_gallery.loading;
}

const GalleryState *gallery_state(void)
{
	return &g_gallery;
}

bool gallery_is_open(void)
{
	return g_gallery.open;
}

void gallery_open(void)
{
	gallery_scan();
	g_gallery.open = true;
	gallery_select(g_gallery.count - 1, GALLERY_FIRST_PICTURE); // the newest
}

void gallery_close(void)
{
	g_gallery.open = false;
}

void gallery_arm_delete(u64 untilMs)
{
	g_gallery.deleteArmedUntil = untilMs;
}

// Only the newest request is loaded: presses made during a load skip the pictures in between.
bool gallery_take_load_request(void)
{
	if (!g_gallery.open || !g_gallery.loading) {
		return false;
	}
	g_gallery.loading = false;
	return true;
}

void gallery_set_load_failed(bool failed)
{
	g_gallery.loadFailed = failed;
}

void gallery_set_stereo_shown(bool shown)
{
	g_gallery.stereoShown = shown;
}

void gallery_adjust_convergence(int change)
{
	g_gallery.convergence = math_clamp_int(g_gallery.convergence + change, -CONVERGENCE_MAX, CONVERGENCE_MAX);
}

// Removes every file it can, even after a failure.
static bool delete_folder_files(const char *folder)
{
	DIR *dir = opendir(folder);
	if (!dir) {
		return true;
	}
	char path[SHOT_FILE_PATH_MAX];
	bool ok = true;
	struct dirent *entry;
	while ((entry = readdir(dir))) {
		if (entry->d_name[0] == '.') {
			continue;
		}
		if (snprintf(path, sizeof path, "%s/%s", folder, entry->d_name) >= (int)sizeof path) {
			ok = false; // a cut-off path would name another file
			continue;
		}
		if (remove(path) != 0) {
			ok = false;
		}
	}
	closedir(dir);
	return ok;
}

static bool delete_folder(const char *folder)
{
	bool ok = delete_folder_files(folder);
	if (rmdir(folder) != 0) {
		ok = false;
	}
	return ok;
}

// The photo and whichever sidecars it has. Done when the photo itself is gone.
static bool delete_photo(int id, const char *photoPath)
{
	static const char *const SUFFIXES[] = {".PNG", ".JPG", ".DNG", ".YUV", ".TXT"};
	char path[SHOT_FILE_PATH_MAX];
	for (int i = 0; i < ARRAY_COUNT(SUFFIXES); i++) {
		snprintf(path, sizeof path, PHOTO_DIR "/IMG_%04d%s", id, SUFFIXES[i]);
		remove(path);
	}
	struct stat status;
	return stat(photoPath, &status) != 0;
}

bool gallery_delete_item(const GalleryItem *item)
{
	char path[SHOT_PATH_MAX];
	item_path(item, path, sizeof path);
	if (item->folder) {
		return delete_folder(path);
	}
	return delete_photo(item->id, path);
}

// The slider sets the depth as on any 3DS. L and R fine-tune it.
int gallery_stereo_depth(void)
{
	float sliderDepth = osGet3DSliderState() * SLIDER_MAX_DEPTH;
	int roundedDepth = (int)(sliderDepth + 0.5f);
	return g_gallery.convergence + roundedDepth;
}

void gallery_refresh_when_saved(int id)
{
	g_refreshShotId = id;
	g_refreshPending = id != -1; // -1: the shot has no number
}

void gallery_refresh_poll(void)
{
	if (!g_refreshPending || !saver_idle()) {
		return;
	}
	g_refreshPending = false;
	if (!g_gallery.open) {
		return;
	}
	int shotId = g_refreshShotId;
	gallery_scan();
	for (int i = 0; i < g_gallery.count; i++) {
		if (g_gallery.items[i].id == shotId) {
			gallery_select(i, GALLERY_FIRST_PICTURE);
		}
	}
}
