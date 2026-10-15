// Learns each camera's hot and dead pixels from the live view, without dark frames.
// A pixel far above all eight same-colour neighbours in a dark area counts as hot, one far below them in a
// lit area as dead. Real detail moves with the camera and stuck pixels stay put, so only frames whose scene
// changed are counted. Portable C, no 3DS calls.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define HOT_PIXEL_CAMERAS 3
#define HOT_PIXEL_MAP_MAX 4096 // pixels in one camera's map
#define HOT_PIXEL_SCENE_SAMPLES 1600 // more than the 40 x 30 scene grid samples of a full frame

// The scene grid of the last frame a feeder had checked. Each feeder keeps its own: the live view one for
// every camera, the burst shots one for camera B.
typedef struct {
	uint16_t green[HOT_PIXEL_SCENE_SAMPLES];
	bool have; // without it the next frame counts as all change
} HotPixelScene;

// Checks a raw frame (RAW10 storage, RGGB) when its scene changed enough since the feeder's last checked
// frame: one slice of its rows, or with wholeFrame all of them. width x height is the frame as streamed, from
// the full frame's top left. fastMode: the 60 fps row-skipping mode (640 x 240).
void hot_pixel_learner_feed_frame(int camera, HotPixelScene *scene, const uint8_t *raw, int width, int height,
                                  bool fastMode, int blackLevel, bool wholeFrame);

// The frame row a full-frame row is streamed on, or -1 when the fast mode skips it.
int hot_pixel_learner_frame_row(int fullRow, bool fastMode);

// The learned map of a camera as x, y pairs in full-frame coordinates. Returns the number of pixels.
int hot_pixel_learner_map(int camera, const uint16_t **xy);

// Loads or saves the counts of every camera as dir/hot_cam<n>.bin. Saving skips cameras with no change.
void hot_pixel_learner_load(const char *dir);
void hot_pixel_learner_save(const char *dir);
