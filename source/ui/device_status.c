#include "device_status.h"
#include <3ds.h>
#include <citro2d.h>
#include "app_threads.h"
#include "cam_settings.h"
#include "saver.h"
#include "settings.h"
#include "ui_text.h"

#define STATUS_INTERVAL_MS 2000
#define CARD_IDLE_BEFORE_SPACE_CHECK_MS 20000
#define SPACE_CHECK_INTERVAL_MS 60000
#define SHOTS_LEFT_MAX 9999 // the counter has four digits

// Card space one photo may take: a full-size uncompressed picture (3 bytes a pixel plus a small header), the
// YUYV frame's own bytes when the stream is not RGB565, and the sidecar text. Every photo size counts as full
// size.
#define FRAME_PIXELS (CAM_FRAME_WIDTH * CAM_FRAME_HEIGHT)
#define PICTURE_BYTES_PER_PIXEL 3
#define PICTURE_HEADER_SIZE_ESTIMATE 54
#define SIDECAR_SIZE_ESTIMATE 1024

#define SPACE_THREAD_STACK_SIZE (8 * 1024)
#define SPACE_THREAD_PRIORITY_BELOW_MAIN 3 // a larger number is a lower priority

#define BATTERY_LEVEL_MAX 5
#define BATTERY_LEVEL_LOW 1

// Battery icon, in pixels from its top left corner.
#define BATTERY_WIDTH 20
#define BATTERY_HEIGHT 10
#define BATTERY_BORDER 1
#define BATTERY_TIP_WIDTH 2
#define BATTERY_TIP_HEIGHT 4
#define BATTERY_TIP_Y ((BATTERY_HEIGHT - BATTERY_TIP_HEIGHT) / 2)
#define BATTERY_BAR_X 3
#define BATTERY_BAR_Y 2
#define BATTERY_BAR_PITCH 3
#define BATTERY_BAR_WIDTH 2 // whole pixels, so every bar looks the same
#define BATTERY_BAR_HEIGHT (BATTERY_HEIGHT - 2 * BATTERY_BAR_Y)

static u8 g_batteryLevel = BATTERY_LEVEL_MAX;
static u8 g_batteryCharging;
static u64 g_statusReadAt;

// After files are written, FAT32 rebuilds its free-cluster count by scanning the whole FAT. That takes
// seconds on a big card, so the free space is read on a low-priority thread, and only while the card is idle.
static Thread g_spaceThread;
static _Atomic bool g_spaceThreadBusy;
static u64 g_spaceCheckedAt;
static bool g_spaceNeverChecked = true;

static void read_free_space(void *arg)
{
	u32 bytesPerShot = (u32)(uintptr_t)arg;
	FS_ArchiveResource resource;
	if (R_SUCCEEDED(FSUSER_GetSdmcArchiveResource(&resource))) {
		u64 freeBytes = (u64)resource.clusterSize * resource.freeClusters;
		u64 shotsLeft = freeBytes / bytesPerShot;
		if (shotsLeft > SHOTS_LEFT_MAX) {
			shotsLeft = SHOTS_LEFT_MAX;
		}
		saver_set_shots_left((int)shotsLeft);
	}
	g_spaceThreadBusy = false;
}

static u32 bytes_per_shot(void)
{
	u32 bytes = FRAME_PIXELS * PICTURE_BYTES_PER_PIXEL + PICTURE_HEADER_SIZE_ESTIMATE + SIDECAR_SIZE_ESTIMATE;
	if (g_settings.cam.format != CAM_FORMAT_RGB565) {
		bytes += CAM_FRAME_BYTES;
	}
	return bytes;
}

static void join_finished_space_thread(void)
{
	if (!g_spaceThread || g_spaceThreadBusy) {
		return;
	}
	threadJoin(g_spaceThread, U64_MAX);
	threadFree(g_spaceThread);
	g_spaceThread = NULL;
}

static bool card_idle_for_space_check(u64 now)
{
	return saver_idle() && now - g_lastSavedAt > CARD_IDLE_BEFORE_SPACE_CHECK_MS &&
	       now - g_spaceCheckedAt > SPACE_CHECK_INTERVAL_MS;
}

static void start_space_thread(u64 now)
{
	g_spaceNeverChecked = false;
	g_spaceCheckedAt = now;
	u32 bytesPerShot = bytes_per_shot();
	s32 mainPriority = MAIN_THREAD_PRIORITY;
	svcGetThreadPriority(&mainPriority, CUR_THREAD_HANDLE);
	s32 priority = mainPriority + SPACE_THREAD_PRIORITY_BELOW_MAIN;
	if (priority > THREAD_PRIORITY_LOWEST) {
		priority = THREAD_PRIORITY_LOWEST;
	}
	g_spaceThreadBusy = true;
	g_spaceThread = threadCreate(read_free_space, (void *)(uintptr_t)bytesPerShot, SPACE_THREAD_STACK_SIZE,
	                             priority, THREAD_CORE_APP, false);
	if (!g_spaceThread) {
		g_spaceThreadBusy = false;
	}
}

void device_status_refresh(bool force)
{
	u64 now = osGetTime();
	if (!force && now - g_statusReadAt < STATUS_INTERVAL_MS) {
		return;
	}
	g_statusReadAt = now;
	PTMU_GetBatteryLevel(&g_batteryLevel);
	PTMU_GetBatteryChargeState(&g_batteryCharging);
	join_finished_space_thread();
	if (g_spaceThread) {
		return;
	}
	if (!g_spaceNeverChecked && !card_idle_for_space_check(now)) {
		return;
	}
	start_space_thread(now);
}

static u32 battery_color(void)
{
	if (g_batteryCharging) {
		return COLOR_ACCENT;
	}
	if (g_batteryLevel <= BATTERY_LEVEL_LOW) {
		return COLOR_RED;
	}
	return COLOR_INK;
}

void device_status_draw_battery(float x, float y, float z)
{
	u32 color = battery_color();
	C2D_DrawRectSolid(x, y, z, BATTERY_WIDTH, BATTERY_HEIGHT, color);
	C2D_DrawRectSolid(x + BATTERY_BORDER, y + BATTERY_BORDER, z, BATTERY_WIDTH - 2 * BATTERY_BORDER,
	                  BATTERY_HEIGHT - 2 * BATTERY_BORDER, COLOR_BLACK);
	C2D_DrawRectSolid(x + BATTERY_WIDTH, y + BATTERY_TIP_Y, z, BATTERY_TIP_WIDTH, BATTERY_TIP_HEIGHT, color);
	int bars = g_batteryLevel > BATTERY_LEVEL_MAX ? BATTERY_LEVEL_MAX : g_batteryLevel;
	for (int i = 0; i < bars; i++) {
		float barX = x + BATTERY_BAR_X + i * BATTERY_BAR_PITCH;
		C2D_DrawRectSolid(barX, y + BATTERY_BAR_Y, z, BATTERY_BAR_WIDTH, BATTERY_BAR_HEIGHT, color);
	}
}
