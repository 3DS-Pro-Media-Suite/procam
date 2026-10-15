#include "hot_pixel_live.h"
#include <3ds.h>
#include "cam_settings.h"
#include "camera_control.h"
#include "fast_view.h"
#include "hot_pixel_learner.h"
#include "math_util.h"
#include "raw_view.h"
#include "saver.h"
#include "settings.h"

#define CELL_SIZE 2 // an RGGB cell is 2 x 2 pixels
#define LEARNER_FRAME_INTERVAL 3 // every third live frame teaches the learner
#define AUTOSAVE_INTERVAL_MS 120000 // 2 minutes

static u32 g_learnerTick;
// One scene for every camera. It starts at black, so a dark first view does not count as a change.
static HotPixelScene g_liveScene = {.have = true};

static int learner_camera(void)
{
	return math_clamp_int(g_settings.cam.camera, 0, HOT_PIXEL_CAMERAS - 1);
}

// The live view only reads the frame, so the learned hot pixels are patched in the frame itself. Each becomes
// the mean of its same-colour neighbours, one cell to the left and right.
static void patch_hot_pixels(u8 *frame, int width, int height, bool fastMode)
{
	const u16 *pixelXY;
	int count = hot_pixel_learner_map(learner_camera(), &pixelXY);
	for (int i = 0; i < count; i++) {
		int x = pixelXY[i * 2];
		int sensorRow = pixelXY[i * 2 + 1];
		int y = hot_pixel_learner_frame_row(sensorRow, fastMode);
		if (y < 0 || y >= height || x < CELL_SIZE || x >= width - CELL_SIZE) {
			continue;
		}
		int left = raw_view_sample(frame, width, y, x - CELL_SIZE);
		int right = raw_view_sample(frame, width, y, x + CELL_SIZE);
		int value = (left + right) / 2;
		raw10_pack(frame + ((size_t)y * width + x) * RAW10_BYTES_PER_PIXEL, value);
	}
}

// The learner works in full-frame coordinates, so it needs full-width frames.
void hot_pixel_live_update(u8 *frame, int width, int height)
{
	bool fastMode = height * 2 == CAM_FRAME_HEIGHT && fast_view_active();
	if (width != CAM_FRAME_WIDTH || (height != CAM_FRAME_HEIGHT && !fastMode)) {
		return;
	}
	g_learnerTick++;
	if (g_learnerTick % LEARNER_FRAME_INTERVAL == 0) {
		hot_pixel_learner_feed_frame(learner_camera(), &g_liveScene, frame, width, height, fastMode,
		                             RAW10_BLACK, false);
	}
	patch_hot_pixels(frame, width, height, fastMode);
}

void hot_pixel_live_autosave_poll(u64 nowMs)
{
	static u64 savedAt;
	if (!savedAt) {
		savedAt = nowMs;
	}
	if (nowMs - savedAt > AUTOSAVE_INTERVAL_MS && saver_idle() && !camera_control_changing()) {
		savedAt = nowMs;
		hot_pixel_learner_save(APP_DIR);
	}
}
