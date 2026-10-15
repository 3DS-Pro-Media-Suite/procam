#include "viewfinder.h"
#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#include "auto_exposure.h"
#include "background_merge.h"
#include "cam_settings.h"
#include "cam_stream.h"
#include "camera_control.h"
#include "device_status.h"
#include "gallery.h"
#include "gallery_view.h"
#include "look_editor.h"
#include "math_util.h"
#include "metering.h"
#include "preview.h"
#include "review.h"
#include "saver.h"
#include "self_timer.h"
#include "settings.h"
#include "shot_mode.h"
#include "shot_names.h"
#include "time_units.h"
#include "toast.h"
#include "ui_layout.h"
#include "ui_shapes.h"
#include "ui_text.h"

#define EDGE_MARGIN 12

#define INK_WITH_ALPHA(alpha) UI_COLOR(238, 235, 228, alpha)
#define COLOR_TRANSPARENT UI_COLOR(0, 0, 0, 0)
#define HISTOGRAM_BACKGROUND_COLOR UI_COLOR(0, 0, 0, 120)
#define HISTOGRAM_BAR_COLOR INK_WITH_ALPHA(200)
#define HISTOGRAM_BASELINE_COLOR INK_WITH_ALPHA(90)
#define EV_TICK_COLOR INK_WITH_ALPHA(160)
#define EV_LINE_COLOR INK_WITH_ALPHA(110)
#define METER_FRAME_COLOR UI_COLOR(255, 178, 56, 230) // the accent, slightly see-through
#define GRID_COLOR UI_COLOR(255, 255, 255, 70)
#define TIMER_DISC_COLOR UI_COLOR(0, 0, 0, 150)
#define TIMER_TRACK_COLOR UI_COLOR(255, 255, 255, 60)
#define PILL_COLOR UI_COLOR(20, 20, 22, 255)

// Messages on a black screen.
#define UNAVAILABLE_TITLE_Y 80
#define UNAVAILABLE_ERROR_Y 118
#define UNAVAILABLE_EXIT_Y 140
#define STARTING_TEXT_Y 110

// The strip along the top: a title, or the HUD's camera, format, size, clock, card and battery.
#define HUD_SHADE_HEIGHT 34
#define TOP_CHIP_Y 6
#define CHIP_HEIGHT 14
#define CHIP_RADIUS 3
#define FORMAT_CHIP_WIDTH 28
#define STEREO_CHIP_WIDTH 22
#define GAP_AFTER_TEXT 10
#define GAP_AFTER_TEXT_NARROW 6 // on the touch screen, so the 3D chip stays clear of the clock
#define GAP_AFTER_CHIP 8
#define SHOTS_LEFT_RIGHT_INSET 30 // from the right edge to the end of the shots-left number
#define BATTERY_RIGHT_INSET 22 // the battery and its tip end at the right edge
#define BATTERY_Y 8
#define MODE_BADGE_Y 26
#define MODE_BADGE_SIZE 48
#define HISTOGRAM_Y 36
#define HISTOGRAM_WIDTH 96
#define HISTOGRAM_HEIGHT 34

// The strip along the bottom: exposure scale, lock chip, metering, exposure and white balance.
#define BOTTOM_SHADE_HEIGHT 44
#define BOTTOM_SHADE_Y (SCREEN_HEIGHT - BOTTOM_SHADE_HEIGHT)
#define BOTTOM_TEXT_Y 219
#define EV_TEXT_Y 200
#define EV_SCALE_Y 218
#define AE_LOCK_CHIP_Y 217
#define AE_LOCK_CHIP_WIDTH 34
#define AE_LOCK_CHIP_HEIGHT 15
#define AE_LOCK_CHIP_GAP 6
#define EXPOSURE_TIME_SIZE 16
#define LONG_EXPOSURE_SECONDS 0.95f // from here on the time shows in seconds, not as a fraction

// The exposure bias scale: one tick per EV.
#define EV_STEP_WIDTH 12.0f
#define EV_STEP_WIDTH_NARROW 10.0f // on the touch screen, so the readouts beside the scale fit
#define EV_SCALE_HEIGHT 8
#define EV_TICK_CENTER_HEIGHT 8
#define EV_TICK_EVEN_HEIGHT 5
#define EV_TICK_ODD_HEIGHT 3
#define EV_SIGN_GAP 6
#define EV_SIGN_OFFSET_Y 1
#define EV_BAR_OFFSET_Y 5
#define EV_BAR_HEIGHT 3
#define EV_POINTER_HALF_WIDTH 4.5f
#define EV_POINTER_RISE 4
#define EV_POINTER_TIP_Y 2

// The histogram sums the preview's bins into fewer, wider bars.
#define HISTOGRAM_BARS 64
#define BINS_PER_BAR (PREVIEW_HISTOGRAM_BINS / HISTOGRAM_BARS)
#define MIN_VISIBLE_BAR_HEIGHT 0.5f
#define CLIP_WARNING_PERCENT 1 // an end bar turns red once this percentage of the pixels clips

#define STEREO_EYE_SHARE 0.5f // each eye moves by half the 3D depth offset
#define METER_FRAME_THICKNESS 1.5f
#define GRID_DIVISIONS 3

#define SHUTTER_FLASH_MS 220
#define SHUTTER_FLASH_MAX_ALPHA 230

#define TIMER_CENTER_Y 112
#define TIMER_DISC_RADIUS 40
#define TIMER_RING_RADIUS 36
#define TIMER_TRACK_THICKNESS 2
#define TIMER_RING_THICKNESS 4
#define TIMER_FULL_TURN 6.2832f // 2 pi to four decimals: the ring's vertices and segment count follow from it
#define TIMER_TEXT_Y 98

// The pill that shows a merge running in the background.
#define MERGE_PILL_WIDTH 120
#define MERGE_PILL_HEIGHT 24
#define MERGE_PILL_RADIUS (MERGE_PILL_HEIGHT / 2)
#define MERGE_PILL_X (TOP_CENTER_X - MERGE_PILL_WIDTH / 2)
#define MERGE_PILL_Y 38
#define MERGE_PILL_TEXT_Y 41
#define MERGE_TRACK_WIDTH 92.0f
#define MERGE_TRACK_HEIGHT 2
#define MERGE_TRACK_RADIUS (MERGE_TRACK_HEIGHT / 2)
#define MERGE_TRACK_X (TOP_CENTER_X - MERGE_TRACK_WIDTH / 2)
#define MERGE_TRACK_Y 56

#define TOAST_Y 166
#define TOAST_TEXT_Y 170
#define TOAST_HEIGHT 22
#define TOAST_RADIUS (TOAST_HEIGHT / 2)
#define TOAST_PADDING_X 12

// The capture status over the live view.
#define CAPTURE_BAR_HEIGHT 3
#define CAPTURE_TEXT_Y 108
#define APPLYING_TEXT_Y 226

// Where the frame lands on the screen and how it is drawn.
typedef struct {
	float x;
	float y;
	float width;
	float height;
	float scale;
	int rowRepeat; // screen rows per frame row
	int flip; // FLIP_* bits applied while drawing
} PictureLayout;

static int g_inset;

void viewfinder_set_inset(int inset)
{
	g_inset = inset;
}

int viewfinder_left_edge(void)
{
	return EDGE_MARGIN + g_inset;
}

int viewfinder_right_edge(void)
{
	return TOP_SCREEN_WIDTH - EDGE_MARGIN - g_inset;
}

// The viewfinder on the touch screen is narrower, so its HUD is spaced tighter.
static bool narrow_viewfinder(void)
{
	return g_inset > 0;
}

// A fast live view frame has half the rows: each one is shown twice.
static int screen_rows_per_frame_row(void)
{
	return gallery_is_open() ? 1 : g_frameRowRepeat;
}

void viewfinder_draw_top_shade(float height)
{
	u32 topColor = COLOR_SHADE;
	u32 bottomColor = COLOR_TRANSPARENT;
	C2D_DrawRectangle(0, 0, DEPTH_OVERLAY, TOP_SCREEN_WIDTH, height, topColor, topColor, bottomColor,
	                  bottomColor);
}

// The bottom strip's shade, like the top one.
static void draw_bottom_shade(void)
{
	u32 topColor = COLOR_TRANSPARENT;
	u32 bottomColor = COLOR_SHADE;
	C2D_DrawRectangle(0, BOTTOM_SHADE_Y, DEPTH_OVERLAY, TOP_SCREEN_WIDTH, BOTTOM_SHADE_HEIGHT, topColor,
	                  topColor, bottomColor, bottomColor);
}

static void draw_histogram(float x, float y, float width, float height)
{
	C2D_DrawRectSolid(x, y, DEPTH_OVERLAY, width, height, HISTOGRAM_BACKGROUND_COLOR);
	u32 barCounts[HISTOGRAM_BARS];
	u32 tallestCount = 1;
	u32 pixelCount = 0;
	for (int bar = 0; bar < HISTOGRAM_BARS; bar++) {
		barCounts[bar] = 0;
		int firstBin = bar * BINS_PER_BAR;
		for (int bin = firstBin; bin < firstBin + BINS_PER_BAR; bin++) {
			barCounts[bar] += g_histogram[bin];
		}
		pixelCount += barCounts[bar];
		// The end bars show clipping, not the picture's tones, so the others set the scale.
		bool innerBar = bar > 0 && bar < HISTOGRAM_BARS - 1;
		if (innerBar && barCounts[bar] > tallestCount) {
			tallestCount = barCounts[bar];
		}
	}
	float barWidth = width / HISTOGRAM_BARS;
	float maxBarHeight = height - 2; // a free pixel above the bars and the baseline below them
	for (int bar = 0; bar < HISTOGRAM_BARS; bar++) {
		float barHeight = maxBarHeight * (float)barCounts[bar] / (float)tallestCount;
		if (barHeight > maxBarHeight) {
			barHeight = maxBarHeight;
		}
		bool endBar = bar == 0 || bar == HISTOGRAM_BARS - 1;
		bool clipping = endBar && pixelCount && barCounts[bar] * 100 / pixelCount >= CLIP_WARNING_PERCENT;
		if (barHeight >= MIN_VISIBLE_BAR_HEIGHT) {
			u32 barColor = clipping ? COLOR_RED : HISTOGRAM_BAR_COLOR;
			ui_shapes_rect(x + bar * barWidth, y + height - 1 - barHeight, barWidth, barHeight, barColor,
			               DEPTH_HISTOGRAM_BARS);
		}
	}
	C2D_DrawRectSolid(x, y + height - 1, DEPTH_HISTOGRAM_BASELINE, width, 1, HISTOGRAM_BASELINE_COLOR);
}

static float ev_tick_height(int stop)
{
	if (stop == 0) {
		return EV_TICK_CENTER_HEIGHT;
	}
	if (stop % 2 == 0) {
		return EV_TICK_EVEN_HEIGHT;
	}
	return EV_TICK_ODD_HEIGHT;
}

// The exposure bias scale: a tick per EV, the bias as a bar from the centre and a pointer above it.
static void draw_ev_scale(float centerX, float y)
{
	float stepWidth = narrow_viewfinder() ? EV_STEP_WIDTH_NARROW : EV_STEP_WIDTH;
	for (int stop = -CAM_EXPOSURE_BIAS_MAX; stop <= CAM_EXPOSURE_BIAS_MAX; stop++) {
		float tickX = centerX + stop * stepWidth;
		float tickHeight = ev_tick_height(stop);
		u32 tickColor = stop == 0 ? COLOR_INK : EV_TICK_COLOR;
		C2D_DrawRectSolid(tickX - 0.5f, y + EV_SCALE_HEIGHT - tickHeight, DEPTH_HUD, 1, tickHeight,
		                  tickColor);
	}
	float scaleLeft = centerX - CAM_EXPOSURE_BIAS_MAX * stepWidth;
	float scaleWidth = 2 * CAM_EXPOSURE_BIAS_MAX * stepWidth + 1;
	C2D_DrawRectSolid(scaleLeft, y + EV_SCALE_HEIGHT, DEPTH_HUD, scaleWidth, 1, EV_LINE_COLOR);
	float scaleRight = centerX + CAM_EXPOSURE_BIAS_MAX * stepWidth;
	float signY = y + EV_SIGN_OFFSET_Y;
	ui_text_draw_shadowed(g_fontSmall, scaleLeft - EV_SIGN_GAP, signY, COLOR_MUTED, TEXT_ALIGN_RIGHT, "-");
	ui_text_draw_shadowed(g_fontSmall, scaleRight + EV_SIGN_GAP, signY, COLOR_MUTED, TEXT_ALIGN_LEFT, "+");
	int bias = g_settings.cam.exposure;
	float markerX = centerX + bias * stepWidth;
	if (bias) {
		float barLeft = bias > 0 ? centerX : markerX;
		float barWidth = bias > 0 ? markerX - centerX : centerX - markerX;
		C2D_DrawRectSolid(barLeft, y + EV_BAR_OFFSET_Y, DEPTH_HUD_BAR, barWidth, EV_BAR_HEIGHT, COLOR_ACCENT);
	}
	float pointerTop = y - EV_POINTER_RISE;
	ui_shapes_triangle(markerX - EV_POINTER_HALF_WIDTH, pointerTop, markerX + EV_POINTER_HALF_WIDTH,
	                   pointerTop, markerX, y + EV_POINTER_TIP_Y, COLOR_ACCENT, DEPTH_EV_POINTER);
}

// The mode as the top strip shows it, with its option: nothing for PHOTO, and 3D shows its blue chip instead.
// Returns false when the shutter will take something else: a photo, or one camera's version of the mode.
static bool format_mode_badge(const Settings *settings, char *out, size_t size)
{
	int mode = math_clamp_int(settings->mode, 0, MODE_COUNT - 1);
	int frames = shot_mode_merge_frames(settings);
	out[0] = 0;
	if (shot_mode_get(settings) != mode) {
		snprintf(out, size, "%s: OUTER CAMERAS ONLY (PHOTO)", MODE_SHORT_NAMES[mode]);
		return false;
	}
	if ((mode == MODE_MERGE || mode == MODE_HDR) && !cam_stream_raw_active()) {
		snprintf(out, size, "%s: NEEDS RAW (PHOTO)", MODE_SHORT_NAMES[mode]);
		return false;
	}
	if (camera_control_dual_lost(settings)) {
		if (mode == MODE_HDR) {
			snprintf(out, size, "HDR: ONE CAMERA (PLAIN MERGE %d)", frames);
		} else if (mode == MODE_MERGE) {
			snprintf(out, size, "MERGE %d: ONE CAMERA ONLY", frames);
		} else {
			snprintf(out, size, "3D: ONE CAMERA STREAMING (SLOW PAIR)");
		}
		return false;
	}
	if (mode == MODE_MERGE && settings->cam.camera == CAM_INNER) {
		snprintf(out, size, "MERGE %d", frames);
	} else if (mode == MODE_MERGE) {
		snprintf(out, size, "MERGE %d+%d", frames, frames);
	} else if (mode == MODE_HDR) {
		snprintf(out, size, "HDR %s  %d+%d", settings_hdr_gap_name(settings), frames, frames);
	} else if (mode == MODE_BRACKET) {
		snprintf(out, size, "BRACKET %s", settings_bracket_series_name(settings));
	}
	return true;
}

static void draw_camera_unavailable(void)
{
	ui_text_draw(g_fontLarge, TOP_CENTER_X, UNAVAILABLE_TITLE_Y, COLOR_INK, TEXT_ALIGN_CENTER,
	             "Camera unavailable");
	ui_text_draw(g_fontSmall, TOP_CENTER_X, UNAVAILABLE_ERROR_Y, COLOR_MUTED, TEXT_ALIGN_CENTER,
	             "cam:u error 0x%08lX", camera_control_error());
	ui_text_draw(g_fontSmall, TOP_CENTER_X, UNAVAILABLE_EXIT_Y, COLOR_MUTED, TEXT_ALIGN_CENTER,
	             "START + SELECT to exit");
}

// The frame fills the screen's height or width, whichever it reaches first.
static PictureLayout picture_layout(void)
{
	PictureLayout layout;
	layout.rowRepeat = screen_rows_per_frame_row();
	layout.scale = (float)TOP_SCREEN_WIDTH / g_previewWidth;
	float heightScale = (float)SCREEN_HEIGHT / (g_previewHeight * layout.rowRepeat);
	if (heightScale < layout.scale) {
		layout.scale = heightScale;
	}
	layout.width = g_previewWidth * layout.scale;
	layout.height = g_previewHeight * layout.rowRepeat * layout.scale;
	layout.x = (TOP_SCREEN_WIDTH - layout.width) * 0.5f;
	layout.y = (SCREEN_HEIGHT - layout.height) * 0.5f;
	// A raw stream is read out unmirrored, so the flip setting is applied while drawing.
	layout.flip = !gallery_is_open() && cam_stream_raw_active() ? g_settings.cam.flip : 0;
	return layout;
}

static void draw_picture(int eye, const PictureLayout *layout)
{
	C3D_Tex *viewTexture = preview_view_texture();
	Tex3DS_SubTexture subTexture = {
	        .width = (u16)g_previewWidth,
	        .height = (u16)g_previewHeight,
	        .left = 0.0f,
	        .top = 1.0f,
	        .right = (float)g_previewWidth / viewTexture->width,
	        .bottom = 1.0f - (float)g_previewHeight / viewTexture->height,
	};
	bool stereo = gallery_is_open() && gallery_state()->stereoShown;
	C3D_Tex *texture = stereo && eye ? preview_right_eye_texture() : viewTexture;
	C2D_Image image = {texture, &subTexture};
	// 3D: the eyes' pictures move apart by the depth offset. A positive one puts the scene behind the screen.
	float shift = 0;
	if (stereo) {
		float eyeShare = eye ? STEREO_EYE_SHARE : -STEREO_EYE_SHARE;
		shift = eyeShare * gallery_stereo_depth();
	}
	// citro2d mirrors a negative scale in place, so the position stays the same.
	float scaleX = layout->flip & FLIP_HORIZONTAL ? -layout->scale : layout->scale;
	float scaleY = (layout->flip & FLIP_VERTICAL ? -layout->scale : layout->scale) * layout->rowRepeat;
	C2D_DrawImageAt(image, layout->x + shift, layout->y, DEPTH_PICTURE, NULL, scaleX, scaleY);
}

// The touch metering area. Its point is in frame coordinates, so it is mirrored like the picture.
static void draw_touch_meter(const PictureLayout *layout, float touchX, float touchY)
{
	float frameX = layout->flip & FLIP_HORIZONTAL ? 1 - touchX : touchX;
	float frameY = layout->flip & FLIP_VERTICAL ? 1 - touchY : touchY;
	float centerX = layout->x + frameX * layout->width;
	float centerY = layout->y + frameY * layout->height;
	float halfWidth = layout->width / METERING_TOUCH_SPOT_DIVISOR;
	float halfHeight = layout->height / METERING_TOUCH_SPOT_DIVISOR;
	ui_shapes_frame(centerX - halfWidth, centerY - halfHeight, halfWidth * 2 + METER_FRAME_THICKNESS,
	                halfHeight * 2 + METER_FRAME_THICKNESS, METER_FRAME_THICKNESS, METER_FRAME_COLOR,
	                DEPTH_METER_FRAME);
}

// The shutter fired: a short white flash that fades out.
static void draw_shutter_flash(u64 now)
{
	u64 sinceFlash = now - review_shutter_flash_at();
	if (sinceFlash >= SHUTTER_FLASH_MS) {
		return;
	}
	int alpha = (int)(SHUTTER_FLASH_MS - sinceFlash) * SHUTTER_FLASH_MAX_ALPHA / SHUTTER_FLASH_MS;
	C2D_DrawRectSolid(0, 0, DEPTH_FLASH, TOP_SCREEN_WIDTH, SCREEN_HEIGHT, UI_COLOR(255, 255, 255, alpha));
}

static void draw_review_title(void)
{
	viewfinder_draw_top_shade(TITLE_SHADE_HEIGHT);
	ui_text_draw_shadowed(g_fontBody, viewfinder_left_edge(), TOP_TEXT_Y, COLOR_INK, TEXT_ALIGN_LEFT, "%s",
	                      look_editor_active() ? g_lastShot : review_name());
	ui_text_draw_shadowed(g_fontSmall, viewfinder_right_edge(), TITLE_SIDE_TEXT_Y, COLOR_ACCENT,
	                      TEXT_ALIGN_RIGHT, "%s", look_editor_active() ? "TEST RENDER" : review_tag());
}

static void draw_grid(const PictureLayout *layout)
{
	for (int i = 1; i < GRID_DIVISIONS; i++) {
		// Whole pixels keep the hairlines crisp.
		float lineX = floorf(layout->x + layout->width * i / GRID_DIVISIONS + 0.5f);
		C2D_DrawRectSolid(lineX, layout->y, DEPTH_OVERLAY, 1, layout->height, GRID_COLOR);
		float lineY = floorf(layout->y + layout->height * i / GRID_DIVISIONS + 0.5f);
		C2D_DrawRectSolid(layout->x, lineY, DEPTH_OVERLAY, layout->width, 1, GRID_COLOR);
	}
}

// A, S or M: who sets the exposure (auto, shutter priority, manual).
static const char *exposure_mode_letter(void)
{
	if (g_settings.cam.exposureMode == CAM_EXPOSURE_MANUAL) {
		return "M";
	}
	if (g_settings.cam.exposureMode == CAM_EXPOSURE_SHUTTER_PRIORITY) {
		return "S";
	}
	return "A";
}

// With raw frames: the time and gain the sensor exposes with right now.
static void draw_sensor_exposure(void)
{
	float seconds = g_exposureNow.seconds;
	char timeText[EXPOSURE_TIME_SIZE];
	if (seconds >= LONG_EXPOSURE_SECONDS) {
		snprintf(timeText, sizeof timeText, "%.1f\"", seconds);
	} else {
		snprintf(timeText, sizeof timeText, "1/%d", (int)(1.0f / seconds + 0.5f));
	}
	u32 color = g_settings.cam.exposureMode == CAM_EXPOSURE_AUTO ? COLOR_INK : COLOR_ACCENT;
	ui_text_draw_shadowed(g_fontSmall, viewfinder_right_edge(), BOTTOM_TEXT_Y, color, TEXT_ALIGN_RIGHT,
	                      "%s  %s s  ISO %d", exposure_mode_letter(), timeText, g_exposureNow.iso);
}

static void draw_exposure_readout(float meterX)
{
	ui_text_draw_shadowed(g_fontSmall, meterX, BOTTOM_TEXT_Y, COLOR_INK, TEXT_ALIGN_LEFT, "%s",
	                      METERING_SHORT_NAMES[(int)g_settings.cam.aeWindow]);
	if (g_exposureNow.valid) {
		draw_sensor_exposure();
	} else {
		ui_text_draw_shadowed(g_fontSmall, viewfinder_right_edge(), BOTTOM_TEXT_Y, COLOR_INK,
		                      TEXT_ALIGN_RIGHT, "WB %s   %s fps",
		                      WHITE_BALANCE_SHORT_NAMES[(int)g_settings.cam.whiteBalance],
		                      FRAME_RATE_SHORT_NAMES[(int)g_settings.cam.frameRate]);
	}
	int bias = g_settings.cam.exposure;
	ui_text_draw_shadowed(g_fontSmall, TOP_CENTER_X, EV_TEXT_Y, bias ? COLOR_ACCENT : COLOR_MUTED,
	                      TEXT_ALIGN_CENTER, bias ? "%+d EV" : "%d EV", bias);
}

static void draw_bottom_strip(void)
{
	if (g_settings.hud >= HUD_MINIMAL) {
		draw_bottom_shade();
		draw_ev_scale(TOP_CENTER_X, EV_SCALE_Y);
	}
	// The lock chip shows at every HUD level: a locked exposure does not follow the scene.
	float meterX = viewfinder_left_edge();
	if (!g_settings.cam.autoExposure) {
		ui_shapes_round_rect(meterX, AE_LOCK_CHIP_Y, AE_LOCK_CHIP_WIDTH, AE_LOCK_CHIP_HEIGHT, CHIP_RADIUS,
		                     COLOR_ACCENT, DEPTH_HUD);
		ui_text_draw(g_fontSmall, meterX + AE_LOCK_CHIP_WIDTH / 2, BOTTOM_TEXT_Y, COLOR_BLACK,
		             TEXT_ALIGN_CENTER, "AE-L");
		meterX += AE_LOCK_CHIP_WIDTH + AE_LOCK_CHIP_GAP;
	}
	if (g_settings.hud >= HUD_MINIMAL) {
		draw_exposure_readout(meterX);
	}
}

static void draw_clock(void)
{
	time_t clockTime = time(NULL);
	struct tm *localTime = localtime(&clockTime);
	ui_text_draw_shadowed(g_fontSmall, TOP_CENTER_X, TOP_TEXT_Y, COLOR_INK, TEXT_ALIGN_CENTER, "%02d:%02d",
	                      localTime->tm_hour, localTime->tm_min);
}

static void draw_top_strip(void)
{
	viewfinder_draw_top_shade(HUD_SHADE_HEIGHT);
	float cameraWidth =
	        ui_text_draw_shadowed(g_fontSmall, viewfinder_left_edge(), TOP_TEXT_Y, COLOR_INK, TEXT_ALIGN_LEFT,
	                              "%s", CAMERA_SHORT_NAMES[(int)g_settings.cam.camera]);
	float textGap = narrow_viewfinder() ? GAP_AFTER_TEXT_NARROW : GAP_AFTER_TEXT;
	float x = viewfinder_left_edge() + cameraWidth + textGap;
	bool coloredFormat = g_settings.cam.format != CAM_FORMAT_RGB565 || cam_stream_raw_active();
	u32 formatColor = coloredFormat ? COLOR_ACCENT : COLOR_MUTED;
	ui_shapes_round_rect(x, TOP_CHIP_Y, FORMAT_CHIP_WIDTH, CHIP_HEIGHT, CHIP_RADIUS, formatColor, DEPTH_HUD);
	ui_text_draw(g_fontSmall, x + FORMAT_CHIP_WIDTH / 2, TOP_TEXT_Y, COLOR_BLACK, TEXT_ALIGN_CENTER, "%s",
	             cam_stream_raw_active() ? "RAW" : OUTPUT_FORMAT_SHORT_NAMES[(int)g_settings.cam.format]);
	x += FORMAT_CHIP_WIDTH + GAP_AFTER_CHIP;
	// The frame size counts a fast live view frame as the full frame it is shown as.
	int shownHeight = g_previewHeight * screen_rows_per_frame_row();
	x += ui_text_draw_shadowed(g_fontSmall, x, TOP_TEXT_Y, COLOR_MUTED, TEXT_ALIGN_LEFT,
	                           "%d" UTF8_MULTIPLICATION_SIGN "%d", g_previewWidth, shownHeight) +
	     textGap;
	if (shot_mode_get(&g_settings) == MODE_3D) {
		ui_shapes_round_rect(x, TOP_CHIP_Y, STEREO_CHIP_WIDTH, CHIP_HEIGHT, CHIP_RADIUS, COLOR_BLUE,
		                     DEPTH_HUD);
		ui_text_draw(g_fontSmall, x + STEREO_CHIP_WIDTH / 2, TOP_TEXT_Y, COLOR_BLACK, TEXT_ALIGN_CENTER,
		             "3D");
	}
	draw_clock();
	int rightEdge = viewfinder_right_edge();
	device_status_draw_battery(rightEdge - BATTERY_RIGHT_INSET, BATTERY_Y, DEPTH_HUD);
	int shotsLeft = saver_shots_left();
	if (shotsLeft >= 0) {
		ui_text_draw_shadowed(g_fontSmall, rightEdge - SHOTS_LEFT_RIGHT_INSET, TOP_TEXT_Y, COLOR_INK,
		                      TEXT_ALIGN_RIGHT, "%d", shotsLeft);
	}
	if (g_settings.histogram) {
		int histogramX = rightEdge - HISTOGRAM_WIDTH;
		draw_histogram(histogramX, HISTOGRAM_Y, HISTOGRAM_WIDTH, HISTOGRAM_HEIGHT);
	}
	char badge[MODE_BADGE_SIZE];
	bool usable = format_mode_badge(&g_settings, badge, sizeof badge);
	if (badge[0]) {
		ui_text_draw_shadowed(g_fontSmall, viewfinder_left_edge(), MODE_BADGE_Y,
		                      usable ? COLOR_ACCENT : COLOR_MUTED, TEXT_ALIGN_LEFT, "%s", badge);
	}
}

// The self-timer: big seconds inside a draining ring.
static void draw_self_timer(u64 now)
{
	u64 start;
	u64 end;
	self_timer_progress(&start, &end);
	float total = (float)(end - start);
	s64 remainingMs = (s64)end - (s64)now;
	float remaining = (float)remainingMs;
	if (remaining < 0) {
		remaining = 0;
	}
	ui_shapes_circle(TOP_CENTER_X, TIMER_CENTER_Y, TIMER_DISC_RADIUS, TIMER_DISC_COLOR, DEPTH_TIMER_DISC);
	ui_shapes_arc(TOP_CENTER_X, TIMER_CENTER_Y, TIMER_RING_RADIUS, 0, TIMER_FULL_TURN, TIMER_TRACK_THICKNESS,
	              TIMER_TRACK_COLOR, DEPTH_TIMER_TRACK);
	ui_shapes_arc(TOP_CENTER_X, TIMER_CENTER_Y, TIMER_RING_RADIUS, 0, TIMER_FULL_TURN * remaining / total,
	              TIMER_RING_THICKNESS, COLOR_ACCENT, DEPTH_TIMER_RING);
	ui_text_draw(g_fontLarge, TOP_CENTER_X, TIMER_TEXT_Y, COLOR_INK, TEXT_ALIGN_CENTER, "%d",
	             (int)(remaining / 1000) + 1);
}

// A MERGE or HDR shot being merged in the background, and how many more shots wait for it.
static void draw_merge_pill(void)
{
	ui_shapes_round_rect(MERGE_PILL_X, MERGE_PILL_Y, MERGE_PILL_WIDTH, MERGE_PILL_HEIGHT, MERGE_PILL_RADIUS,
	                     PILL_COLOR, DEPTH_PILL);
	const char *what = background_merge_is_hdr() ? "HDR" : "MERGING";
	if (background_merge_waiting()) {
		ui_text_draw(g_fontSmall, TOP_CENTER_X, MERGE_PILL_TEXT_Y, COLOR_ACCENT, TEXT_ALIGN_CENTER,
		             "%s  %d%%  +%d", what, background_merge_progress(), background_merge_waiting());
	} else {
		ui_text_draw(g_fontSmall, TOP_CENTER_X, MERGE_PILL_TEXT_Y, COLOR_ACCENT, TEXT_ALIGN_CENTER,
		             "%s  %d%%", what, background_merge_progress());
	}
	ui_shapes_round_rect(MERGE_TRACK_X, MERGE_TRACK_Y, MERGE_TRACK_WIDTH, MERGE_TRACK_HEIGHT,
	                     MERGE_TRACK_RADIUS, COLOR_FAINT, DEPTH_MERGE_TRACK);
	if (background_merge_progress() > 0) {
		ui_shapes_round_rect(MERGE_TRACK_X, MERGE_TRACK_Y,
		                     MERGE_TRACK_WIDTH * background_merge_progress() / 100, MERGE_TRACK_HEIGHT,
		                     MERGE_TRACK_RADIUS, COLOR_ACCENT, DEPTH_MERGE_FILL);
	}
}

// The toast: an opaque pill above the bottom strip.
static void draw_toast(void)
{
	C2D_Text text;
	C2D_TextFontParse(&text, g_fontSmall, g_textBuffer, g_toastText);
	float scale = ui_text_font_scale(g_fontSmall);
	float width;
	float height;
	C2D_TextGetDimensions(&text, scale, scale, &width, &height);
	ui_shapes_round_rect(TOP_CENTER_X - width / 2 - TOAST_PADDING_X, TOAST_Y, width + 2 * TOAST_PADDING_X,
	                     TOAST_HEIGHT, TOAST_RADIUS, PILL_COLOR, DEPTH_PILL);
	ui_text_draw_string(g_fontSmall, TOP_CENTER_X, TOAST_TEXT_Y, 1, COLOR_INK, TEXT_ALIGN_CENTER, 0,
	                    g_toastText);
}

void viewfinder_draw(int eye)
{
	C2D_DrawRectSolid(0, 0, 0, TOP_SCREEN_WIDTH, SCREEN_HEIGHT, COLOR_BLACK);
	if (!camera_control_ok()) {
		draw_camera_unavailable();
		return;
	}
	if (!g_haveFrame) {
		ui_text_draw(g_fontBody, TOP_CENTER_X, STARTING_TEXT_Y, COLOR_MUTED, TEXT_ALIGN_CENTER,
		             "Starting camera");
		return;
	}
	u64 now = osGetTime();
	bool reviewShown = review_showing(now);
	PictureLayout layout = picture_layout();
	draw_picture(eye, &layout);
	bool galleryOpen = gallery_is_open();
	float touchX;
	float touchY;
	if (!galleryOpen && !reviewShown && metering_touch_point(&touchX, &touchY)) {
		draw_touch_meter(&layout, touchX, touchY);
	}
	if (galleryOpen) {
		gallery_view_draw_overlay();
		return;
	}
	draw_shutter_flash(now);
	if (reviewShown) {
		draw_review_title();
		return;
	}
	if (g_settings.grid && g_settings.hud) {
		draw_grid(&layout);
	}
	draw_bottom_strip();
	if (g_settings.hud >= HUD_FULL) {
		draw_top_strip();
	}
	if (self_timer_counting()) {
		draw_self_timer(now);
	}
	if (background_merge_active()) {
		draw_merge_pill();
	}
	if (now < g_toastUntil) {
		draw_toast();
	}
}

// A capture in progress, shown whether the shutter sounds or not, or settings being applied.
void viewfinder_draw_camera_status(void)
{
	if (camera_control_shooting_since() && !gallery_is_open()) {
		C2D_DrawRectSolid(0, 0, DEPTH_HUD, TOP_SCREEN_WIDTH, CAPTURE_BAR_HEIGHT, COLOR_RED);
		C2D_DrawRectSolid(0, SCREEN_HEIGHT - CAPTURE_BAR_HEIGHT, DEPTH_HUD, TOP_SCREEN_WIDTH,
		                  CAPTURE_BAR_HEIGHT, COLOR_RED);
		static const char *const SHOT_NAMES[MODE_COUNT] = {"photo", "merge burst", "HDR burst", "3D pair",
		                                                   "bracket"};
		int mode = math_clamp_int(camera_control_shooting_mode(), 0, MODE_COUNT - 1);
		ui_text_draw_shadowed(g_fontBody, TOP_CENTER_X, CAPTURE_TEXT_Y, COLOR_RED, TEXT_ALIGN_CENTER,
		                      "Capturing %s - hold still  %.1f s", SHOT_NAMES[mode],
		                      (osGetTime() - camera_control_shooting_since()) /
		                              (double)MILLISECONDS_PER_SECOND);
	} else if (camera_control_applying() && !gallery_is_open()) {
		ui_text_draw_shadowed(g_fontSmall, TOP_CENTER_X, APPLYING_TEXT_Y, COLOR_ACCENT, TEXT_ALIGN_CENTER,
		                      "Applying camera settings...");
	}
}
