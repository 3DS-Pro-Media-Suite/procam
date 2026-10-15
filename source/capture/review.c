#include "review.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cam_settings.h"
#include "gallery.h"
#include "preview.h"
#include "raw10.h"
#include "raw_view.h"
#include "saver.h"
#include "shot_names.h"

#define REVIEW_MAX_PIXELS (CAM_FRAME_WIDTH * CAM_FRAME_HEIGHT)
#define REVIEW_TAG_SIZE 32

// The camera thread posts a copy of each photo's frame here and the main loop takes it. The camera thread
// runs above the main thread on the same core, so the mailbox is only touched under its lock.
typedef struct {
	LightLock lock;
	u8 *pixels; // the newest photo's frame, CAM_FRAME_BYTES at most
	int width;
	int height;
	int format; // CAM_FORMAT_*
	char name[sizeof g_lastShot]; // "IMG_0042"
	char tag[REVIEW_TAG_SIZE]; // "SAVED", or "MERGING" for the source frame of a merge
	bool hasNewFrame; // pixels is newer than the main thread's frame
	bool hasPhoto; // a SELECT tap can show it again
	int showMs; // > 0: a request to show it for this long
} ReviewMailbox;

static ReviewMailbox g_reviewMailbox;

// Main thread: the review's frame, traded for the mailbox's frame when a newer one is taken.
static u8 *g_reviewPixels;
static int g_reviewWidth;
static int g_reviewHeight;
static int g_reviewFormat;
static char g_reviewName[sizeof g_lastShot];
static char g_reviewTag[REVIEW_TAG_SIZE];
static u64 g_reviewShownUntil;

static u64 g_shutterFlashAt; // when the shutter fired, for the viewfinder flash

// Main thread, at the start. False when memory ran out.
bool review_init(void)
{
	snprintf(g_reviewTag, sizeof g_reviewTag, "SAVED");
	LightLock_Init(&g_reviewMailbox.lock);
	g_reviewMailbox.pixels = malloc(CAM_FRAME_BYTES);
	g_reviewPixels = malloc(CAM_FRAME_BYTES);
	return g_reviewMailbox.pixels && g_reviewPixels;
}

// After camera_control_exit: the camera thread has ended, so nothing posts any more.
void review_exit(void)
{
	free(g_reviewMailbox.pixels);
	free(g_reviewPixels);
}

static int round_raw_level(float level)
{
	if (level < 0) {
		return 0;
	}
	if (level > RAW10_WHITE) {
		return RAW10_WHITE;
	}
	return (int)(level + 0.5f);
}

// Multiplies an 8+2 raw frame's levels above black by gain, per RGGB channel.
static void scale_raw_levels(u8 *pixels, int width, int height, float gain,
                             const float blackLevels[CFA_CELLS])
{
	for (int y = 0; y < height; y++) {
		u8 *sample = pixels + (size_t)y * width * RAW10_BYTES_PER_PIXEL;
		for (int x = 0; x < width; x++) {
			int channel = (y & 1) << 1 | (x & 1); // R, Gr, Gb, B
			float channelBlack = blackLevels[channel];
			int level = raw10_unpack(sample);
			int scaled = round_raw_level(channelBlack + (level - channelBlack) * gain);
			raw10_pack(sample, scaled);
			sample += RAW10_BYTES_PER_PIXEL;
		}
	}
}

// Camera thread: the photo just taken, named as saver_next_shot_path named it last. showMs 0 keeps it for a
// SELECT tap only. A raw frame's levels can be scaled by gain: the HDR review brings camera B's frame to the
// metered exposure. tag: the label beside the name.
void review_post_scaled(const u8 *pixels, int width, int height, int format, int showMs, float gain,
                        const float blackLevels[CFA_CELLS], const char *tag)
{
	if (!pixels || width < 1 || height < 1 || width * height > REVIEW_MAX_PIXELS) {
		return;
	}
	LightLock_Lock(&g_reviewMailbox.lock);
	if (g_reviewMailbox.pixels) {
		memcpy(g_reviewMailbox.pixels, pixels, (size_t)width * height * CAM_PIXEL_BYTES);
		// Under the lock: the main thread may take the copy as soon as it is let go.
		if (format == CAM_FORMAT_RAW && blackLevels && gain != 1) {
			scale_raw_levels(g_reviewMailbox.pixels, width, height, gain, blackLevels);
		}
		g_reviewMailbox.width = width;
		g_reviewMailbox.height = height;
		g_reviewMailbox.format = format;
		snprintf(g_reviewMailbox.name, sizeof g_reviewMailbox.name, "%s", g_lastShot);
		snprintf(g_reviewMailbox.tag, sizeof g_reviewMailbox.tag, "%s", tag);
		g_reviewMailbox.hasNewFrame = true;
		g_reviewMailbox.hasPhoto = true;
		if (showMs > 0) {
			g_reviewMailbox.showMs = showMs;
		}
	}
	LightLock_Unlock(&g_reviewMailbox.lock);
}

void review_post(const u8 *pixels, int width, int height, int format, int showMs)
{
	review_post_scaled(pixels, width, height, format, showMs, 1, NULL, "SAVED");
}

// Main thread: a merged shot's review frame gets the merge's outcome as its label.
void review_retag(const char *name, const char *tag)
{
	LightLock_Lock(&g_reviewMailbox.lock);
	if (!strcmp(g_reviewMailbox.name, name)) {
		snprintf(g_reviewMailbox.tag, sizeof g_reviewMailbox.tag, "%s", tag);
	}
	LightLock_Unlock(&g_reviewMailbox.lock);
	if (!strcmp(g_reviewName, name)) {
		snprintf(g_reviewTag, sizeof g_reviewTag, "%s", tag);
	}
}

// Shows the last photo posted, at the end of a shot or on a SELECT tap. False when there is none yet.
bool review_request(int showMs)
{
	LightLock_Lock(&g_reviewMailbox.lock);
	bool hasPhoto = g_reviewMailbox.hasPhoto;
	if (hasPhoto) {
		g_reviewMailbox.showMs = showMs;
	}
	LightLock_Unlock(&g_reviewMailbox.lock);
	return hasPhoto;
}

// Main loop: how long a requested review shows, or 0. The newest photo becomes the main thread's by trading
// buffers under the lock, so the conversion runs outside it. Over the gallery a request is dropped: the
// review would replace the gallery's picture in the texture.
static int take_review_request(void)
{
	LightLock_Lock(&g_reviewMailbox.lock);
	int showMs = gallery_is_open() ? 0 : g_reviewMailbox.showMs;
	g_reviewMailbox.showMs = 0;
	if (showMs > 0 && g_reviewMailbox.hasNewFrame && g_reviewPixels) {
		u8 *spareBuffer = g_reviewPixels;
		g_reviewPixels = g_reviewMailbox.pixels;
		g_reviewMailbox.pixels = spareBuffer;
		g_reviewWidth = g_reviewMailbox.width;
		g_reviewHeight = g_reviewMailbox.height;
		g_reviewFormat = g_reviewMailbox.format;
		memcpy(g_reviewName, g_reviewMailbox.name, sizeof g_reviewName);
		g_reviewMailbox.hasNewFrame = false;
	}
	// The tag can change after the frame is taken.
	if (showMs > 0 && !strcmp(g_reviewName, g_reviewMailbox.name)) {
		memcpy(g_reviewTag, g_reviewMailbox.tag, sizeof g_reviewTag);
	}
	LightLock_Unlock(&g_reviewMailbox.lock);
	return showMs;
}

// Main loop: a requested review replaces the live view for its time.
void review_poll(void)
{
	int showMs = take_review_request();
	if (showMs <= 0) {
		return;
	}
	const bool isLiveFrame = false;
	preview_idle();
	preview_ensure_texture(g_reviewWidth, g_reviewHeight);
	preview_process(g_reviewPixels, g_reviewFormat, g_reviewWidth, g_reviewHeight, preview_staging_stride(),
	                isLiveFrame);
	preview_upload(g_reviewWidth, g_reviewHeight);
	review_show_for(showMs);
}

// The picture in the preview texture (a review, or the look editor's test render) stays for ms.
void review_show_for(int ms)
{
	g_reviewShownUntil = osGetTime() + ms;
}

// Any key ends the review: the live view comes back.
void review_hide(void)
{
	g_reviewShownUntil = 0;
}

bool review_showing(u64 now)
{
	return now < g_reviewShownUntil;
}

// The label of the review on screen: its photo's name and what it is.
const char *review_name(void)
{
	return g_reviewName;
}

const char *review_tag(void)
{
	return g_reviewTag;
}

// The shutter fired: the viewfinder flashes.
void review_flash_shutter(void)
{
	g_shutterFlashAt = osGetTime();
}

u64 review_shutter_flash_at(void)
{
	return g_shutterFlashAt;
}
