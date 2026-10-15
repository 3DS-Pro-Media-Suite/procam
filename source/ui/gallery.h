// The gallery list: scans the photo folder, selects an item or a picture inside a folder, deletes items.
#pragma once
#include <3ds.h>
#include "shot_info.h"

#define GALLERY_MAX_ITEMS 4000
#define GALLERY_MAX_FILES 48
#define GALLERY_FILE_NAME_SIZE 24
#define GALLERY_PATH_MAX 96
#define GALLERY_FIRST_PICTURE (-1) // gallery_select: start at the folder's first picture

enum {
	GALLERY_PICTURE_PNG,
	GALLERY_PICTURE_JPG,
	GALLERY_PICTURE_DNG // a RAW-only shot
};

// A single photo or a multi-frame shot's folder.
typedef struct {
	int id; // the shot number, up to SHOT_NUMBER_MAX: more than a u16 holds
	u8 folder;
	u8 pictureType; // GALLERY_PICTURE_*
} GalleryItem;

// What the gallery shows. Other modules read it through gallery_state and change it through the gallery_*
// functions.
typedef struct {
	GalleryItem items[GALLERY_MAX_ITEMS]; // ascending by number
	int count;
	int index;
	bool open;
	bool loading; // the picture of path waits for the loader
	bool loadFailed;
	// The current folder's pictures, in display order.
	char files[GALLERY_MAX_FILES][GALLERY_FILE_NAME_SIZE];
	int fileCount;
	int fileIndex;
	ShotInfo info; // the current folder's INFO.TXT
	char path[GALLERY_PATH_MAX]; // the picture on screen, or being loaded
	char rightPath[GALLERY_PATH_MAX]; // the right eye's picture of the 3D entry, else empty
	bool stereoShown; // both eyes are loaded: the top screen shows the pair in 3D
	int convergence; // horizontal offset between the eyes in screen pixels, set with L / R
	u64 deleteArmedUntil; // delete asks twice: the first press arms it for a few seconds
} GalleryState;

const GalleryState *gallery_state(void);
bool gallery_is_open(void);

void gallery_scan(void);
// Shows item index. file: the picture inside a folder to start at, or GALLERY_FIRST_PICTURE.
void gallery_select(int index, int file);
void gallery_open(void);
void gallery_close(void);
// Deletes everything of one item: the photo with its sidecars, or the whole folder. False when the photo, or
// a file of the folder, could not be deleted.
bool gallery_delete_item(const GalleryItem *item);
// Arms the delete until untilMs (osGetTime). 0 disarms it.
void gallery_arm_delete(u64 untilMs);

// The loader takes the picture of path and rightPath to load. False while none waits.
bool gallery_take_load_request(void);
// The loader's outcome for the picture on screen.
void gallery_set_load_failed(bool failed);
void gallery_set_stereo_shown(bool shown);

// L and R move the eyes of the 3D view apart or together.
void gallery_adjust_convergence(int change);
// The 3D view's offset between the eyes: the 3D slider plus the convergence.
int gallery_stereo_depth(void);
// A merged photo went to the saver: the gallery lists and selects it once it is on the card.
void gallery_refresh_when_saved(int id);
// Main loop: re-lists the gallery when a merged photo has been saved.
void gallery_refresh_poll(void);
