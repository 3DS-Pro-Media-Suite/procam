#include "look_test_render.h"
#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include "app_threads.h"
#include "cam_settings.h"
#include "look_render.h"
#include "preview.h"
#include "raw10.h"
#include "raw_view.h"
#include "review.h"
#include "toast.h"
#include "white_balance.h"

#define TEST_RENDER_SHOW_MS 8000 // how long the test render replaces the live view
#define TEST_RENDER_STACK_SIZE (32 * 1024)
#define TEST_RENDER_PRIORITY_BELOW_MAIN 1 // a larger number is a lower priority

typedef struct {
	Thread thread;
	_Atomic bool busy;
	_Atomic bool done;
	int lookIndex;
	u8 *raw;
	u8 *rgb;
	int width;
	int height;
	float whiteBalance[RGB_CHANNELS];
	u64 durationMs;
} TestRender;

static TestRender g_testRender;

static void free_buffers(void)
{
	free(g_testRender.raw);
	free(g_testRender.rgb);
	g_testRender.raw = NULL;
	g_testRender.rgb = NULL;
}

static void render_thread(void *unused)
{
	(void)unused;
	u64 startMs = osGetTime();
	if (!look_render(g_testRender.raw, g_testRender.width, g_testRender.height, RAW10_BLACK, RAW10_WHITE,
	                 g_testRender.whiteBalance, g_testRender.lookIndex, g_testRender.rgb)) {
		free(g_testRender.rgb);
		g_testRender.rgb = NULL;
	}
	g_testRender.durationMs = osGetTime() - startMs;
	g_testRender.done = true;
	g_testRender.busy = false;
}

void look_test_render_start(int lookIndex)
{
	if (g_testRender.busy || g_testRender.done) {
		return;
	}
	int frameWidth;
	int frameHeight;
	int frameFormat;
	const void *frame = preview_last_frame(&frameWidth, &frameHeight, &frameFormat);
	if (!frame || frameFormat != CAM_FORMAT_RAW) {
		toast_show("The test render needs the raw live view");
		return;
	}
	size_t pixelCount = (size_t)frameWidth * frameHeight;
	size_t rawSize = pixelCount * RAW10_BYTES_PER_PIXEL;
	g_testRender.raw = malloc(rawSize);
	g_testRender.rgb = malloc(pixelCount * RGB_CHANNELS);
	if (!g_testRender.raw || !g_testRender.rgb) {
		free_buffers();
		toast_show("Not enough memory for a test render");
		return;
	}
	memcpy(g_testRender.raw, frame, rawSize); // the camera refills its buffers: render from a copy
	g_testRender.width = frameWidth;
	g_testRender.height = frameHeight;
	g_testRender.lookIndex = lookIndex;
	white_balance_get(g_testRender.whiteBalance);
	g_testRender.done = false;
	g_testRender.busy = true;
	s32 priority = MAIN_THREAD_PRIORITY;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	int renderPriority = priority + TEST_RENDER_PRIORITY_BELOW_MAIN;
	g_testRender.thread =
	        threadCreate(render_thread, NULL, TEST_RENDER_STACK_SIZE, renderPriority, THREAD_CORE_APP, false);
	if (!g_testRender.thread) {
		render_thread(NULL);
	}
}

// Copies the finished render into the live view's texture and shows it on the top screen.
static void show_render(void)
{
	int width = g_testRender.width;
	int height = g_testRender.height;
	preview_idle();
	preview_ensure_texture(width, height);
	for (int y = 0; y < height; y++) {
		const u8 *source = g_testRender.rgb + (size_t)y * width * RGB_CHANNELS;
		u32 *destination = preview_staging_row(y);
		for (int x = 0; x < width; x++) {
			destination[x] = RGBA(source[RGB_RED], source[RGB_GREEN], source[RGB_BLUE]);
			source += RGB_CHANNELS;
		}
	}
	// It is the same raw frame as the live view, so it is mirrored the same way.
	preview_upload(width, height);
	review_show_for(TEST_RENDER_SHOW_MS);
	toast_show("Test render, every step (%llu ms). Any key: live view",
	           (unsigned long long)g_testRender.durationMs);
}

bool look_test_render_poll(void)
{
	if (g_testRender.busy) {
		return true;
	}
	if (g_testRender.thread) {
		threadJoin(g_testRender.thread, U64_MAX);
		threadFree(g_testRender.thread);
		g_testRender.thread = NULL;
	}
	if (!g_testRender.done) {
		return false;
	}
	g_testRender.done = false;
	if (g_testRender.rgb && g_testRender.width <= RAW_VIEW_MAX_WIDTH) {
		show_render();
	} else {
		toast_show("The test render failed (memory)");
	}
	free_buffers();
	return false;
}

bool look_test_render_busy(void)
{
	return g_testRender.busy;
}
