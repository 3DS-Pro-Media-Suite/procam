// Pro Cam: a camera with manual controls.
//
// Top screen: the live view with histogram, framing grid, zebra and focus peaking. Bottom screen: the Q menu,
// the setup menu (START), the gallery and an info page. A takes what the MODE setting says and saves it to
// PHOTO_DIR. The sensors are read raw through a copy of the camera module's i2c session. Without it (Citra,
// or when the copy fails) the camera service's YUV stream is used: photos, brackets and 3D pairs as PNG, no
// MERGE or HDR.
#include <3ds.h>
#include <citro2d.h>
#include <sys/stat.h>
#include "auto_exposure.h"
#include "background_merge.h"
#include "cam_stream.h"
#include "camera_control.h"
#include "device_status.h"
#include "exit_screen.h"
#include "ftp.h"
#include "gallery.h"
#include "gallery_loader.h"
#include "hot_pixel_learner.h"
#include "hot_pixel_live.h"
#include "input.h"
#include "look_test_render.h"
#include "preview.h"
#include "review.h"
#include "saver.h"
#include "screens.h"
#include "self_timer.h"
#include "sensor_color_matrix.h"
#include "settings.h"
#include "time_units.h"
#include "toast.h"
#include "ui_text.h"
#include "f_big_bcfnt.h"
#include "f_body_bcfnt.h"
#include "f_small_bcfnt.h"

#define TEXT_BUFFER_GLYPHS 6144
// Anti-aliased shapes take more triangles than the default allows.
#define DRAW_OBJECT_LIMIT (C2D_DEFAULT_MAX_OBJECTS * 2)

// Only Citra answers this system info type with 1.
#define CITRA_INFO_TYPE 0x20000
#define CITRA_INFO_IS_CITRA 0

#define SHUTDOWN_POLL_NS (10 * NANOSECONDS_PER_MILLISECOND)

static void start_graphics(Screens *screens)
{
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(DRAW_OBJECT_LIMIT);
	C2D_Prepare();
	screens->top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
	screens->topRight = C2D_CreateScreenTarget(GFX_TOP, GFX_RIGHT);
	screens->bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
	g_textBuffer = C2D_TextBufNew(TEXT_BUFFER_GLYPHS);
	g_fontSmall = C2D_FontLoadFromMem(f_small_bcfnt, f_small_bcfnt_size);
	g_fontBody = C2D_FontLoadFromMem(f_body_bcfnt, f_body_bcfnt_size);
	g_fontLarge = C2D_FontLoadFromMem(f_big_bcfnt, f_big_bcfnt_size);
}

static void free_fonts(void)
{
	C2D_TextBufDelete(g_textBuffer);
	if (g_fontSmall) {
		C2D_FontFree(g_fontSmall);
	}
	if (g_fontBody) {
		C2D_FontFree(g_fontBody);
	}
	if (g_fontLarge) {
		C2D_FontFree(g_fontLarge);
	}
}

static bool running_on_citra(void)
{
	s64 isCitra = 0;
	Result result = svcGetSystemInfo(&isCitra, CITRA_INFO_TYPE, CITRA_INFO_IS_CITRA);
	return R_SUCCEEDED(result) && isCitra == 1;
}

// A first run has no app folder. The looks, the hot pixels and the settings live there.
static void load_saved_state(void)
{
	mkdir(HOMEBREW_DIR, FOLDER_MODE);
	mkdir(APP_DIR, FOLDER_MODE);
	hot_pixel_learner_load(APP_DIR);
	// The looks come first: loading the settings clamps the LOOK value to the looks there are.
	settings_load_looks();
	settings_load();
}

static void poll_raw_exposure(void)
{
	if (cam_stream_raw_active() && !camera_control_changing()) {
		u64 nowMs = osGetTime();
		sensor_color_matrix_poll(g_settings.cam.camera);
		auto_exposure_poll(&g_settings.cam, nowMs);
		camera_control_poll_stream(nowMs);
	} else if (!cam_stream_raw_active()) {
		auto_exposure_clear();
	}
}

static void poll_modules(void)
{
	self_timer_poll();
	review_poll();
	gallery_refresh_poll();
	poll_raw_exposure();
	gallery_loader_poll();
	look_test_render_poll();
	hot_pixel_live_autosave_poll(osGetTime());
	device_status_refresh(false);
}

static void report_failures(void)
{
	camera_control_watchdog();
	Result cameraFailure = camera_control_take_failure();
	if (cameraFailure) {
		toast_show("Camera error 0x%08lX", (unsigned long)cameraFailure);
	}
	int saveFailures = saver_take_failures();
	if (saveFailures) {
		const char *plural = saveFailures > 1 ? "s" : "";
		toast_show("Could not save %d photo%s to the SD card", saveFailures, plural);
	}
}

static void stop_modules(const Screens *screens)
{
	gallery_close();
	while (gallery_loader_poll()) {
		svcSleepThread(SHUTDOWN_POLL_NS);
	}
	camera_control_exit();
	// The camera thread has ended. Stopping the sensors keeps them idle during the merges below and, on an
	// Old 3DS, takes the capture thread off the merge's core.
	if (camera_control_ok()) {
		cam_stream_stop();
	}
	exit_screen_finish_merges(screens);
	preview_exit();
	// Writes the photos still in the queue.
	saver_exit();
	hot_pixel_learner_save(APP_DIR);
	if (camera_control_ok()) {
		settings_save();
	}
	cam_stream_exit();
	ftp_exit();
}

int main(void)
{
	Screens screens;
	start_graphics(&screens);
	ptmuInit();
	APT_CheckNew3DS(&g_isNew3ds);
	osSetSpeedupEnable(true);
	bool onCitra = running_on_citra();

	bool previewReady = preview_init();
	bool reviewReady = review_init();
	load_saved_state();
	saver_init();
	background_merge_init();
	camera_control_init();
	camera_control_start(onCitra, previewReady && reviewReady);

	static aptHookCookie aptCookie;
	aptHook(&aptCookie, camera_control_apt_hook, NULL);

	bool quit = false;
	while (aptMainLoop() && !quit) {
		camera_control_fire_pending();
		background_merge_poll();
		// Starts the next MERGE or HDR shot once the merge slot is free.
		background_merge_start();
		// The mode can also change through a preset or a reset.
		settings_update_mode_row();
		hidScanInput();
		input_handle(&quit);
		poll_modules();
		bool liveViewShown = !review_showing(osGetTime()) && !gallery_is_open();
		preview_feed_from_camera(liveViewShown);
		screens_draw_frame(&screens);
		report_failures();
	}

	aptUnhook(&aptCookie);
	stop_modules(&screens);
	review_exit();
	free_fonts();
	ptmuExit();
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	return 0;
}
