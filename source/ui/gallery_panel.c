#include "gallery_panel.h"
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include "background_merge.h"
#include "ftp.h"
#include "gallery.h"
#include "shot_info.h"
#include "saver.h"
#include "settings.h"
#include "shot_names.h"
#include "toast.h"
#include "touch.h"
#include "ui_layout.h"
#include "ui_panel.h"
#include "ui_shapes.h"
#include "ui_text.h"

// The item's name, its kind and the picture of a folder.
#define ITEM_NAME_Y 26
#define ITEM_KIND_Y 46
#define FOLDER_PICTURE_Y 62
#define FOLDER_PICTURE_TOUCH_Y 56
#define FOLDER_PICTURE_TOUCH_HEIGHT 22
#define KIND_TEXT_SIZE 64
#define COUNT_NOTE_SIZE 32 // "n of m" for any two ints

// Previous and next buttons.
#define PAGER_Y 84
#define PAGER_WIDTH 64
#define PAGER_HEIGHT 58
#define PAGER_RADIUS 8
#define PAGER_ARROW_SIZE 10
#define PREVIOUS_X 8
#define NEXT_X 248

// Delete button, between them.
#define DELETE_X 80
#define DELETE_Y 100
#define DELETE_WIDTH 160
#define DELETE_HEIGHT 26
#define DELETE_RADIUS 7
#define DELETE_TEXT_Y (DELETE_Y + 5)
#define MERGE_PROGRESS_Y (DELETE_Y + DELETE_HEIGHT + 6)

// Back and FTP buttons.
#define BACK_X 8
#define FTP_X 216
#define LOWER_BUTTON_Y 168
#define LOWER_BUTTON_WIDTH 96
#define LOWER_BUTTON_HEIGHT 32
#define LOWER_BUTTON_RADIUS 8
#define LOWER_BUTTON_TEXT_Y (LOWER_BUTTON_Y + 7)

#define STATUS_Y 150
#define HELP_Y 208
#define KEYS_Y 222

#define DELETE_CONFIRM_MS 3000
#define CONVERGENCE_STEP 2

#define FTP_PORT 5021 // the PC import script connects to it

static const FtpConfig FTP_CONFIG = {
        .greeting = "Pro Cam photos",
        .rootPath = PHOTO_DIR,
        .port = FTP_PORT,
        .removeFolderContents = true, // a multi-frame shot's folder is one photo
        .canDelete = NULL,
        .isBusy = background_merge_path_busy, // a shot may still get files while it is merged
};

static const char *photo_note(int pictureType)
{
	if (pictureType == GALLERY_PICTURE_DNG) {
		return " (RAW only: DNG, shown in the Standard look)";
	}
	if (pictureType == GALLERY_PICTURE_PNG) {
		return " (PNG)";
	}
	return "";
}

static void kind_text(const GalleryItem *item, char *text, size_t size)
{
	if (!item->folder) {
		snprintf(text, size, "Photo%s", photo_note(item->pictureType));
		return;
	}
	const ShotInfo *info = &gallery_state()->info;
	if (!strcmp(info->kind, "bracket")) {
		snprintf(text, size, "Exposure bracket: %d shots", info->frames);
	} else if (!strcmp(info->kind, "3d")) {
		snprintf(text, size, info->dng ? "3D pair (raw: L, R)" : "3D pair (SBS, L, R)");
	} else {
		snprintf(text, size, "Folder");
	}
}

static void draw_title_bar(void)
{
	const GalleryState *gallery = gallery_state();
	char countNote[COUNT_NOTE_SIZE];
	snprintf(countNote, sizeof countNote, "%d of %d", gallery->index + 1, gallery->count);
	ui_panel_draw_header('G', "GALLERY", gallery->count ? countNote : NULL, COLOR_FAINT);
}

static void draw_item_text(const GalleryItem *item)
{
	const GalleryState *gallery = gallery_state();
	ui_text_draw(g_fontBody, BOTTOM_CENTER_X, ITEM_NAME_Y, COLOR_INK, TEXT_ALIGN_CENTER, SHOT_NAME_FORMAT,
	             item->id);
	char kind[KIND_TEXT_SIZE];
	kind_text(item, kind, sizeof kind);
	ui_text_draw(g_fontSmall, BOTTOM_CENTER_X, ITEM_KIND_Y, COLOR_MUTED, TEXT_ALIGN_CENTER, "%s", kind);
	if (item->folder && gallery->fileCount) {
		ui_text_draw(g_fontSmall, BOTTOM_CENTER_X, FOLDER_PICTURE_Y, COLOR_ACCENT, TEXT_ALIGN_CENTER,
		             "%s   picture %d of %d   (up / down)", gallery->files[gallery->fileIndex],
		             gallery->fileIndex + 1, gallery->fileCount);
	}
}

static void draw_pager_button(float x, int direction)
{
	ui_shapes_round_rect(x, PAGER_Y, PAGER_WIDTH, PAGER_HEIGHT, PAGER_RADIUS, COLOR_TILE, DEPTH_PANEL);
	u32 arrowColor = gallery_state()->count > 1 ? COLOR_INK : COLOR_FAINT;
	ui_shapes_pointer(x + PAGER_WIDTH / 2, PAGER_Y + PAGER_HEIGHT / 2, PAGER_ARROW_SIZE, direction,
	                  arrowColor, DEPTH_CONTROL);
}

static void draw_delete_button(const GalleryItem *item)
{
	bool armed = osGetTime() < gallery_state()->deleteArmedUntil;
	bool deletable = item && !background_merge_shot_busy(item->id) && !saver_shot_saving(item->id);
	u32 buttonColor = armed ? COLOR_RED : COLOR_TILE;
	ui_shapes_round_rect(DELETE_X, DELETE_Y, DELETE_WIDTH, DELETE_HEIGHT, DELETE_RADIUS, buttonColor,
	                     DEPTH_PANEL);
	u32 textColor = COLOR_FAINT;
	if (armed) {
		textColor = COLOR_BLACK;
	} else if (deletable) {
		textColor = COLOR_RED;
	}
	const char *label = armed ? "Tap again to delete" : "Delete (X)";
	ui_text_draw(g_fontBody, DELETE_X + DELETE_WIDTH / 2, DELETE_TEXT_Y, textColor, TEXT_ALIGN_CENTER, label);
}

// A MERGE or HDR shot on its way to the card.
static void draw_merge_progress(void)
{
	if (!background_merge_active()) {
		return;
	}
	const char *action = background_merge_is_hdr() ? "HDR merging" : "Merging";
	ui_text_draw(g_fontSmall, BOTTOM_CENTER_X, MERGE_PROGRESS_Y, COLOR_ACCENT, TEXT_ALIGN_CENTER,
	             "%s " SHOT_NAME_FORMAT ": %d%%", action, background_merge_shot_id(),
	             background_merge_progress());
}

static void draw_back_button(void)
{
	ui_shapes_round_rect(BACK_X, LOWER_BUTTON_Y, LOWER_BUTTON_WIDTH, LOWER_BUTTON_HEIGHT, LOWER_BUTTON_RADIUS,
	                     COLOR_TILE_SELECTED, DEPTH_PANEL);
	ui_text_draw(g_fontBody, BACK_X + LOWER_BUTTON_WIDTH / 2, LOWER_BUTTON_TEXT_Y, COLOR_ACCENT,
	             TEXT_ALIGN_CENTER, "Back");
}

static void draw_ftp_button(bool ftpOn)
{
	u32 buttonColor = ftpOn ? COLOR_ACCENT : COLOR_TILE;
	ui_shapes_round_rect(FTP_X, LOWER_BUTTON_Y, LOWER_BUTTON_WIDTH, LOWER_BUTTON_HEIGHT, LOWER_BUTTON_RADIUS,
	                     buttonColor, DEPTH_PANEL);
	u32 textColor = ftpOn ? COLOR_BLACK : COLOR_INK;
	ui_text_draw(g_fontBody, FTP_X + LOWER_BUTTON_WIDTH / 2, LOWER_BUTTON_TEXT_Y, textColor,
	             TEXT_ALIGN_CENTER, ftpOn ? "FTP on" : "FTP off");
}

// A toast, else the FTP address while the server runs.
static void draw_status_line(bool ftpOn, const char *ftpAddress)
{
	char toastText[TOAST_TEXT_SIZE];
	if (toast_current(osGetTime(), toastText)) {
		ui_text_draw(g_fontSmall, BOTTOM_CENTER_X, STATUS_Y, COLOR_RED, TEXT_ALIGN_CENTER, "%s", toastText);
	} else if (ftpAddress) {
		ui_text_draw(g_fontSmall, BOTTOM_CENTER_X, STATUS_Y, COLOR_ACCENT, TEXT_ALIGN_CENTER, "ftp://%s",
		             ftpAddress);
	} else if (ftpOn) {
		ui_text_draw(g_fontSmall, BOTTOM_CENTER_X, STATUS_Y, COLOR_ACCENT, TEXT_ALIGN_CENTER,
		             "FTP: waiting for Wi-Fi");
	}
}

static void draw_help_lines(void)
{
	const char *help = gallery_state()->stereoShown
	                           ? "3D view: L / R set the depth   up / down other pictures"
	                           : "Left / right photos   up / down pictures in a folder";
	ui_text_draw(g_fontSmall, BOTTOM_CENTER_X, HELP_Y, COLOR_FAINT, TEXT_ALIGN_CENTER, help);
	ui_text_draw(g_fontSmall, BOTTOM_CENTER_X, KEYS_Y, COLOR_FAINT, TEXT_ALIGN_CENTER,
	             "X delete   Y FTP   B back");
}

void gallery_panel_draw(void)
{
	draw_title_bar();
	const GalleryState *gallery = gallery_state();
	const GalleryItem *item = gallery->count ? &gallery->items[gallery->index] : NULL;
	if (item) {
		draw_item_text(item);
	}
	draw_pager_button(PREVIOUS_X, POINTER_DIRECTION_LEFT);
	draw_pager_button(NEXT_X, POINTER_DIRECTION_RIGHT);
	draw_delete_button(item);
	draw_merge_progress();
	draw_back_button();
	bool ftpOn = ftp_running();
	const char *ftpAddress = ftpOn ? ftp_address() : NULL;
	draw_ftp_button(ftpOn);
	draw_status_line(ftpOn, ftpAddress);
	draw_help_lines();
}

// The first press arms the delete, a second one within DELETE_CONFIRM_MS deletes the current item.
static void delete_current_item(void)
{
	const GalleryState *gallery = gallery_state();
	if (!gallery->count) {
		return;
	}
	const GalleryItem *item = &gallery->items[gallery->index];
	if (background_merge_shot_busy(item->id)) {
		toast_show(SHOT_NAME_FORMAT " is being merged", item->id);
		return;
	}
	if (saver_shot_saving(item->id)) {
		toast_show(SHOT_NAME_FORMAT " is still being saved", item->id);
		return;
	}
	u64 now = osGetTime();
	if (now >= gallery->deleteArmedUntil) {
		gallery_arm_delete(now + DELETE_CONFIRM_MS);
		return;
	}
	gallery_arm_delete(0);
	int id = item->id;
	int index = gallery->index;
	if (gallery_delete_item(item)) {
		toast_show("Deleted " SHOT_NAME_FORMAT, id);
	} else {
		toast_show("Could not delete all of " SHOT_NAME_FORMAT, id);
	}
	gallery_scan();
	int lastIndex = gallery->count - 1;
	gallery_select(index < gallery->count ? index : lastIndex, GALLERY_FIRST_PICTURE);
}

static void toggle_ftp(void)
{
	if (ftp_running()) {
		ftp_stop();
		toast_show("FTP server stopped");
	} else if (ftp_start(&FTP_CONFIG)) {
		toast_show("FTP server on");
	} else {
		toast_show("FTP could not start: Wi-Fi off or not enough memory");
	}
}

// Wraps around at either end.
static void step_item(int step)
{
	const GalleryState *gallery = gallery_state();
	if (gallery->count <= 1) {
		return;
	}
	int index = (gallery->index + gallery->count + step) % gallery->count;
	gallery_select(index, GALLERY_FIRST_PICTURE);
}

static void step_folder_picture(int step)
{
	const GalleryState *gallery = gallery_state();
	bool inFolder = gallery->count && gallery->items[gallery->index].folder;
	if (!inFolder || gallery->fileCount <= 1) {
		return;
	}
	int file = (gallery->fileIndex + gallery->fileCount + step) % gallery->fileCount;
	gallery_select(gallery->index, file);
}

void gallery_panel_input(u32 down, touchPosition touch)
{
	// Convergence: R pushes the scene back, L brings it forward.
	if (gallery_state()->stereoShown && (down & (KEY_L | KEY_R))) {
		gallery_adjust_convergence(down & KEY_R ? CONVERGENCE_STEP : -CONVERGENCE_STEP);
		return;
	}
	int itemStep = 0;
	int pictureStep = 0;
	bool touched = down & KEY_TOUCH;
	bool ftpTouched =
	        touched && touch_in_rect(touch, FTP_X, LOWER_BUTTON_Y, LOWER_BUTTON_WIDTH, LOWER_BUTTON_HEIGHT);
	bool deleteTouched = touched && touch_in_rect(touch, DELETE_X, DELETE_Y, DELETE_WIDTH, DELETE_HEIGHT);
	bool pictureLineTouched = touch.py >= FOLDER_PICTURE_TOUCH_Y &&
	                          touch.py < FOLDER_PICTURE_TOUCH_Y + FOLDER_PICTURE_TOUCH_HEIGHT;
	if ((down & KEY_Y) || ftpTouched) {
		toggle_ftp();
	} else if ((down & KEY_X) || deleteTouched) {
		delete_current_item();
	} else if (down & (KEY_B | KEY_START)) {
		gallery_close();
	} else if (down & KEY_DRIGHT) {
		itemStep = 1;
	} else if (down & KEY_DLEFT) {
		itemStep = -1;
	} else if (down & KEY_DDOWN) {
		pictureStep = 1;
	} else if (down & KEY_DUP) {
		pictureStep = -1;
	} else if (touched) {
		if (touch_in_rect(touch, BACK_X, LOWER_BUTTON_Y, LOWER_BUTTON_WIDTH, LOWER_BUTTON_HEIGHT)) {
			gallery_close();
		} else if (touch_in_rect(touch, PREVIOUS_X, PAGER_Y, PAGER_WIDTH, PAGER_HEIGHT)) {
			itemStep = -1;
		} else if (touch_in_rect(touch, NEXT_X, PAGER_Y, PAGER_WIDTH, PAGER_HEIGHT)) {
			itemStep = 1;
		} else if (pictureLineTouched) {
			pictureStep = 1;
		}
	}
	if (itemStep) {
		step_item(itemStep);
	}
	if (pictureStep) {
		step_folder_picture(pictureStep);
	}
}
