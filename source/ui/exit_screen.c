#include "exit_screen.h"
#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include "background_merge.h"
#include "time_units.h"
#include "ui_layout.h"
#include "ui_text.h"

#define POLL_NS (10 * NANOSECONDS_PER_MILLISECOND)
#define ESTIMATE_MIN_PERCENT 5 // a time estimate needs this much progress
#define ESTIMATE_TEXT_SIZE 48
#define TEXT_Y 96
#define DETAIL_Y 122

// The running merge's time so far over its progress, for it and for each one waiting.
static void format_estimate(int percent, char *text, size_t size)
{
	if (percent < ESTIMATE_MIN_PERCENT) {
		snprintf(text, size, "estimating the time");
		return;
	}
	u64 elapsedMs = background_merge_elapsed_ms();
	u64 perShotMs = elapsedMs * 100 / percent;
	u64 remainingMs = perShotMs - elapsedMs + (u64)background_merge_waiting() * perShotMs;
	u64 remainingSeconds = remainingMs / MILLISECONDS_PER_SECOND + 1;
	snprintf(text, size, "about %llu s left", (unsigned long long)remainingSeconds);
}

static void draw_progress(const Screens *screens, int shotsLeft, int percent)
{
	char estimate[ESTIMATE_TEXT_SIZE];
	format_estimate(percent, estimate, sizeof estimate);
	C2D_TextBufClear(g_textBuffer);
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	C2D_TargetClear(screens->top, COLOR_BLACK);
	C2D_SceneBegin(screens->top);
	if (shotsLeft > 1) {
		ui_text_draw(g_fontBody, TOP_CENTER_X, TEXT_Y, COLOR_INK, TEXT_ALIGN_CENTER,
		             "Finishing %d photos before exiting...", shotsLeft);
	} else {
		ui_text_draw(g_fontBody, TOP_CENTER_X, TEXT_Y, COLOR_INK, TEXT_ALIGN_CENTER,
		             "Finishing the last photo before exiting...");
	}
	ui_text_draw(g_fontSmall, TOP_CENTER_X, DETAIL_Y, COLOR_MUTED, TEXT_ALIGN_CENTER, "merging %d %%, %s",
	             percent, estimate);
	C2D_TargetClear(screens->bottom, COLOR_BLACK);
	C3D_FrameEnd(0);
}

// The APT events (HOME, sleep, power) are still served.
void exit_screen_finish_merges(const Screens *screens)
{
	int shownShotsLeft = -1;
	int shownPercent = -1;
	for (;;) {
		int shotsLeft = background_merge_shots_left();
		if (!shotsLeft) {
			break;
		}
		// A close request still waits: the photos exist nowhere else.
		aptMainLoop();
		if (shotsLeft != shownShotsLeft ||
		    (background_merge_active() && background_merge_progress() != shownPercent)) {
			shownShotsLeft = shotsLeft;
			shownPercent = background_merge_active() ? background_merge_progress() : 0;
			draw_progress(screens, shotsLeft, shownPercent);
		}
		// The merge waits while the save queue is full. The sleep gives the saver its time.
		background_merge_poll();
		background_merge_start();
		svcSleepThread(POLL_NS);
	}
}
