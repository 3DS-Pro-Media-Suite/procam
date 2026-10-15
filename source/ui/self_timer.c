#include "self_timer.h"
#include <3ds.h>
#include "camera_control.h"
#include "settings.h"
#include "toast.h"

// The delay of each Settings.timer value: off, 2 s, 5 s, 10 s.
static const int SELF_TIMER_DELAYS_MS[SELF_TIMER_CHOICE_COUNT] = {0, 2000, 5000, 10000};

static u64 g_timerStart;
static u64 g_timerEnd;
static bool g_timerCounting;

void self_timer_press(void)
{
	if (g_timerCounting) {
		g_timerCounting = false;
		toast_show("Self-timer cancelled");
		return;
	}
	int delayMs = SELF_TIMER_DELAYS_MS[(int)g_settings.timer];
	if (delayMs) {
		g_timerCounting = true;
		g_timerStart = osGetTime();
		g_timerEnd = g_timerStart + delayMs;
	} else {
		camera_control_request_shot();
	}
}

void self_timer_poll(void)
{
	if (g_timerCounting && osGetTime() >= g_timerEnd) {
		g_timerCounting = false;
		camera_control_request_shot();
	}
}

bool self_timer_counting(void)
{
	return g_timerCounting;
}

void self_timer_progress(u64 *start, u64 *end)
{
	*start = g_timerStart;
	*end = g_timerEnd;
}
