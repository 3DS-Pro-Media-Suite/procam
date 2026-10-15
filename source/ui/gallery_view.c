#include "gallery_view.h"
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include "background_merge.h"
#include "gallery.h"
#include "gallery_loader.h"
#include "shot_names.h"
#include "ui_layout.h"
#include "ui_text.h"
#include "viewfinder.h"

#define GALLERY_INFO_X 92
#define GALLERY_STATUS_RIGHT_X 300
#define GALLERY_INFO_GAP 6 // between the picture info and the status
#define GALLERY_INFO_SIZE 64
#define GALLERY_EMPTY_TEXT_Y 108
#define GALLERY_ERROR_TEXT_Y 100
#define GALLERY_MERGE_BAR_HEIGHT 6
#define GALLERY_MERGE_BAR_Y (SCREEN_HEIGHT - GALLERY_MERGE_BAR_HEIGHT)
#define GALLERY_MERGE_PIXELS_PER_PERCENT (TOP_SCREEN_WIDTH / 100)
#define GALLERY_MERGE_TEXT_Y 218

// The loading or 3D status, right-aligned. Returns where it starts.
static float draw_gallery_status(const GalleryState *gallery)
{
	float width = 0;
	if (gallery_loader_busy() || gallery->loading) {
		width = ui_text_draw_shadowed(g_fontSmall, GALLERY_STATUS_RIGHT_X, TITLE_SIDE_TEXT_Y, COLOR_MUTED,
		                              TEXT_ALIGN_RIGHT, "Loading...");
	} else if (gallery->stereoShown) {
		width = ui_text_draw_shadowed(
		        g_fontSmall, GALLERY_STATUS_RIGHT_X, TITLE_SIDE_TEXT_Y, COLOR_ACCENT, TEXT_ALIGN_RIGHT,
		        osGet3DSliderState() > 0 ? "3D  depth %+d" : "3D: raise the slider", gallery_stereo_depth());
	}
	return GALLERY_STATUS_RIGHT_X - width;
}

static void draw_gallery_title(const GalleryItem *item)
{
	const GalleryState *gallery = gallery_state();
	int leftEdge = viewfinder_left_edge();
	int rightEdge = viewfinder_right_edge();
	ui_text_draw_shadowed(g_fontBody, leftEdge, TOP_TEXT_Y, COLOR_INK, TEXT_ALIGN_LEFT, SHOT_NAME_FORMAT,
	                      item->id);
	ui_text_draw_shadowed(g_fontSmall, rightEdge, TITLE_SIDE_TEXT_Y, COLOR_ACCENT, TEXT_ALIGN_RIGHT,
	                      "%d / %d", gallery->index + 1, gallery->count);
	float statusLeftX = draw_gallery_status(gallery);
	// A folder also names the picture shown; the size is the picture's full size.
	char info[GALLERY_INFO_SIZE];
	if (item->folder && gallery->fileCount) {
		snprintf(info, sizeof info, "%s  %dx%d", gallery->files[gallery->fileIndex], g_gallerySourceWidth,
		         g_gallerySourceHeight);
	} else {
		snprintf(info, sizeof info, "%dx%d", g_gallerySourceWidth, g_gallerySourceHeight);
	}
	float infoMaxWidth = statusLeftX - GALLERY_INFO_GAP - GALLERY_INFO_X;
	ui_text_draw_shadowed_string(g_fontSmall, GALLERY_INFO_X, TITLE_SIDE_TEXT_Y, COLOR_MUTED, TEXT_ALIGN_LEFT,
	                             infoMaxWidth, info);
}

static void draw_gallery_error(const GalleryItem *item)
{
	const GalleryState *gallery = gallery_state();
	C2D_DrawRectSolid(0, 0, DEPTH_GALLERY_BLANK, TOP_SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
	const char *format = item->folder && !gallery->fileCount ? SHOT_NAME_FORMAT " has no pictures yet"
	                                                         : "Cannot read " SHOT_NAME_FORMAT;
	ui_text_draw(g_fontBody, TOP_CENTER_X, GALLERY_ERROR_TEXT_Y, COLOR_MUTED, TEXT_ALIGN_CENTER, format,
	             item->id);
}

// A merge in progress shows wherever the gallery is.
static void draw_gallery_merge_bar(void)
{
	C2D_DrawRectSolid(0, GALLERY_MERGE_BAR_Y, DEPTH_HUD, TOP_SCREEN_WIDTH, GALLERY_MERGE_BAR_HEIGHT,
	                  COLOR_TILE);
	C2D_DrawRectSolid(0, GALLERY_MERGE_BAR_Y, DEPTH_HUD_BAR,
	                  GALLERY_MERGE_PIXELS_PER_PERCENT * background_merge_progress(),
	                  GALLERY_MERGE_BAR_HEIGHT, COLOR_ACCENT);
	ui_text_draw_shadowed(g_fontSmall, TOP_CENTER_X, GALLERY_MERGE_TEXT_Y, COLOR_ACCENT, TEXT_ALIGN_CENTER,
	                      "%s " SHOT_NAME_FORMAT ": %d%%",
	                      background_merge_is_hdr() ? "HDR merging" : "Merging", background_merge_shot_id(),
	                      background_merge_progress());
}

void gallery_view_draw_overlay(void)
{
	const GalleryState *gallery = gallery_state();
	viewfinder_draw_top_shade(TITLE_SHADE_HEIGHT);
	const GalleryItem *item = gallery->count ? &gallery->items[gallery->index] : NULL;
	if (!item) {
		C2D_DrawRectSolid(0, 0, DEPTH_GALLERY_BLANK, TOP_SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
		ui_text_draw(g_fontBody, TOP_CENTER_X, GALLERY_EMPTY_TEXT_Y, COLOR_MUTED, TEXT_ALIGN_CENTER,
		             "No photos yet");
	} else if (gallery->loadFailed) {
		draw_gallery_error(item);
	} else {
		draw_gallery_title(item);
	}
	if (background_merge_active()) {
		draw_gallery_merge_bar();
	}
}
