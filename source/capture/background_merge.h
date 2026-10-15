// Queues MERGE and HDR bursts, merges them on a worker thread and saves the result.
#pragma once
#include <3ds.h>
#include <time.h>
#include "cam_settings.h"
#include "merge.h"
#include "photo_writer.h"
#include "raw10.h"
#include "still_capture.h"

#define MERGE_NOTE_SIZE 192

// One MERGE or HDR shot on its way to the merge. It owns its frames and defect maps.
// HDR: camera A runs at linesA, camera B at linesB, both at the stream's gain. meteredLines is the live
// exposure at the press, which the JPG is rendered for. Each real time is its lines plus fineLines: only then
// are the ratios the real exposure ratios.
typedef struct {
	int id; // the shot's IMG number
	char basePath[SHOT_PATH_MAX]; // the shot's path without extension: the saver creates its files
	u8 *framesA[MERGE_MAX_FRAMES]; // frame buffers: 8+2 bits as streamed (packed), or u16 once unpacked
	u8 *framesB[MERGE_MAX_FRAMES];
	int frameCountA; // camera A: OUT1, or the inner camera
	int frameCountB; // camera B: OUT2, the left eye. HDR: B_k is taken with A_k
	bool packed;
	int cameraA; // camera A's CamId: its defect map and colour matrix
	const char *captureMethod; // for the log
	RawShotInfo info;
	CamSettings camSettings;
	int exposureBias; // EV
	int gain;
	int iso;
	int lookIndex;
	int photoFormat;
	int sizeDivisor;
	float exposureSeconds; // camera A's time. HDR: the longer one
	float whiteBalance[RGB_CHANNELS];
	float blackLevelsA[CFA_CELLS]; // black level of each CFA cell (10-bit)
	float blackLevelsB[CFA_CELLS];
	u16 *hotPixelsA; // defect maps: x, y pairs, or NULL
	u16 *hotPixelsB;
	int hotPixelCountA;
	int hotPixelCountB;
	time_t when;
	u32 captureMs; // from the shutter press to the last frame
	bool hdrWanted; // the HDR mode took the shot
	bool hdr; // the frames are an HDR burst. Else a plain merge, and note says why
	int hdrGapEv;
	u16 meteredLines;
	u16 linesA;
	u16 linesB;
	float fineLines; // sensor_exposure_fine_lines at the press
	char note[MERGE_NOTE_SIZE]; // what went otherwise than the mode asked, or ""
} MergeRequest;

void background_merge_init(void);
// Frees the request's frames and defect maps.
void background_merge_free_request(MergeRequest *request);
// Queues a copy of the request: its buffers go with it. False when the queue is full.
bool background_merge_push(const MergeRequest *request);
// The camera thread asks this before it takes a burst.
bool background_merge_queue_full(void);
// The shot is merging or waits for it: its files must not be deleted yet.
bool background_merge_shot_busy(int id);
// The same for the shot a path belongs to.
bool background_merge_path_busy(const char *path);
// Main loop: starts the next queued merge when none runs.
void background_merge_start(void);
// Main loop: hands a finished merge to the saver once the save queue has room.
void background_merge_poll(void);

// The merge running now and the shots waiting for it, for the screens that show the progress.
bool background_merge_active(void);
bool background_merge_is_hdr(void);
int background_merge_shot_id(void);
int background_merge_progress(void);
u64 background_merge_elapsed_ms(void);
int background_merge_waiting(void);
int background_merge_shots_left(void);
