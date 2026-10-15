#include "button_hints.h"
#include <3ds.h>
#include <stdio.h>
#include "cam_settings.h"
#include "cam_stream.h"
#include "camera_control.h"
#include "settings.h"
#include "shot_mode.h"
#include "ui_layout.h"
#include "ui_shapes.h"
#include "ui_text.h"
#include "viewfinder.h"

#define MAX_HINTS 16
#define SHUTTER_HINT_SIZE 48
#define HINT_PANEL_MARGIN 8
#define HINT_PANEL_RADIUS 8
#define HINT_PADDING_Y 7
#define HINT_LINE_HEIGHT 14
#define HINT_TEXT_X 10
#define HINT_ACTION_COLUMN 0.42f // where the action column starts, as a share of the panel width
#define HINT_PANEL_COLOR UI_COLOR(0, 0, 0, 205)

typedef struct {
	const char *button;
	const char *action;
} ButtonHint;

// What the shutter takes in the chosen mode.
static void format_shutter_hint(char *out, size_t size)
{
	int mode = shot_mode_for_press(&g_settings);
	int frames = shot_mode_merge_frames(&g_settings);
	bool oneCamera = camera_control_dual_lost(&g_settings);
	if (oneCamera && mode == MODE_HDR) {
		snprintf(out, size, "Shutter: plain merge %d (no HDR)", frames);
	} else if (oneCamera && mode == MODE_MERGE) {
		snprintf(out, size, "Shutter: merge %d, one camera", frames);
	} else if (oneCamera && mode == MODE_3D) {
		snprintf(out, size, "Shutter: 3D pair, the slow way (~3 s)");
	} else if (mode == MODE_MERGE && g_settings.cam.camera == CAM_INNER) {
		snprintf(out, size, "Shutter: merge %d (hold still)", frames);
	} else if (mode == MODE_MERGE) {
		snprintf(out, size, "Shutter: merge %d+%d (hold still)", frames, frames);
	} else if (mode == MODE_HDR) {
		snprintf(out, size, "Shutter: HDR %d+%d, gap %s", frames, frames, settings_hdr_gap_name(&g_settings));
	} else if (mode == MODE_3D) {
		snprintf(out, size, "Shutter: 3D pair");
	} else if (mode == MODE_BRACKET) {
		snprintf(out, size, "Shutter: bracket %s EV", settings_bracket_series_name(&g_settings));
	} else {
		snprintf(out, size, "Shutter: photo");
	}
}

static void add_hint(ButtonHint *hints, int *count, const char *button, const char *action)
{
	hints[*count].button = button;
	hints[*count].action = action;
	(*count)++;
}

static void draw_hint_panel(const ButtonHint *hints, int count)
{
	int leftEdge = viewfinder_left_edge();
	int rightEdge = viewfinder_right_edge();
	float height = 2 * HINT_PADDING_Y + count * HINT_LINE_HEIGHT;
	float top = SCREEN_CENTER_Y - height / 2;
	float left = leftEdge + HINT_PANEL_MARGIN;
	float width = rightEdge - leftEdge - 2 * HINT_PANEL_MARGIN;
	ui_shapes_round_rect(left, top, width, height, HINT_PANEL_RADIUS, HINT_PANEL_COLOR, DEPTH_HINTS);
	for (int i = 0; i < count; i++) {
		float lineY = top + HINT_PADDING_Y + i * HINT_LINE_HEIGHT;
		ui_text_draw(g_fontSmall, left + HINT_TEXT_X, lineY, COLOR_ACCENT, TEXT_ALIGN_LEFT, "%s",
		             hints[i].button);
		ui_text_draw(g_fontSmall, left + width * HINT_ACTION_COLUMN, lineY, COLOR_INK, TEXT_ALIGN_LEFT, "%s",
		             hints[i].action);
	}
}

void button_hints_draw(void)
{
	// With raw frames in S or M, X / Y change the exposure time instead of the bias.
	bool timeDial = cam_stream_raw_active() && g_settings.cam.exposureMode != CAM_EXPOSURE_AUTO;
	char shutterHint[SHUTTER_HINT_SIZE];
	format_shutter_hint(shutterHint, sizeof shutterHint);
	ButtonHint hints[MAX_HINTS];
	int count = 0;
	add_hint(hints, &count, g_isNew3ds ? "A / R / ZR" : "A / R", shutterHint);
	add_hint(hints, &count, "X / Y", timeDial ? "Exposure time: longer / shorter" : "EV: brighter / darker");
	if (timeDial) {
		add_hint(hints, &count, "Circle Pad up / down",
		         g_settings.cam.exposureMode == CAM_EXPOSURE_MANUAL ? "ISO" : "EV");
	}
	add_hint(hints, &count, "Circle Pad left / right", "Turn the selected setting");
	add_hint(hints, &count, "D-pad", "Select a setting");
	add_hint(hints, &count, "L", g_settings.cam.autoExposure ? "Lock the exposure" : "Unlock the exposure");
	add_hint(hints, &count, "B", "Information display");
	add_hint(hints, &count, "SELECT tap / hold", "Last photo / these hints");
	add_hint(hints, &count, g_isNew3ds ? "SELECT + L, ZL" : "SELECT + L", "Swap screens");
	add_hint(hints, &count, "START", "Menu (gallery, settings)");
	if (g_isNew3ds) {
		add_hint(hints, &count, "C-stick", timeDial ? "As X / Y, up / down as the Circle Pad" : "As X / Y");
	}
	if (g_screensSwapped) {
		add_hint(hints, &count, "Touch the picture", "Meter there (again: off)");
	}
	draw_hint_panel(hints, count);
}
