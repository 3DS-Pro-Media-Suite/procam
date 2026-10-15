// Writes one save job to the card: DNG, JPG, PNG, YUV and the sidecar text.
#pragma once
#include <3ds.h>
#include <time.h>
#include "cam_settings.h"
#include "raw10.h"

#define SHOT_PATH_MAX 80 // a shot's path without extension, or its folder
#define SHOT_FILE_PATH_MAX 112 // one file of a shot: the shot's path, a suffix and an extension

// What a job writes. NAME is the job's path without its extension.
typedef enum {
	// frameA, a camera frame: NAME.PNG, NAME.YUV for a YUYV frame, NAME.TXT.
	// A raw frame: NAME.DNG and NAME.JPG as the photo format says, NAME.TXT.
	SAVE_FRAME,
	// path is the shot's folder, frameA the left and frameB the right frame (YUYV): SBS.PNG, L.PNG, R.PNG,
	// L.YUV, R.YUV, INFO.TXT. Raw frames: L.DNG, R.DNG, L.JPG and R.JPG unless RAW only, INFO.TXT.
	SAVE_PAIR,
	// path is a text file, note its content.
	SAVE_TEXT,
	// frameA, a merged RGGB mosaic of u16 values with its defect pixels already fixed: NAME.DNG and
	// NAME.JPG as the photo format says, NAME.TXT with the merge log.
	SAVE_MERGED,
} SaveKind;

typedef struct {
	SaveKind kind;
	char path[SHOT_PATH_MAX];
	u8 *frameA; // owned
	u8 *frameB; // owned
	int format; // CAM_FORMAT_*
	int width;
	int height;
	int sizeDivisor; // the PNG or JPG is this many times smaller: 1, 2 or 4 (nearest neighbour)
	float whiteBalance[RGB_CHANNELS]; // raw: the white balance at the shot, the DNG's as-shot neutral
	float blackLevels[CFA_CELLS]; // raw: black levels R, Gr, Gb, B for the shot's gain
	bool hotFix; // raw: fix hot pixels in the raw before the DNG is written
	float exposureSeconds; // raw: exposure time read back from the sensor (0 = unknown)
	int iso;
	int photoFormat; // raw: PHOTO_FORMAT_*
	int lookIndex; // raw: the JPG's look
	CamSettings camSettings;
	int exposureBias; // EV
	char *note; // owned, may be NULL: appended to the sidecar text (SAVE_TEXT: the whole file)
	char *mergeLog; // owned, may be NULL: SAVE_MERGED's merge log, for the TXT only
	int camera; // SAVE_MERGED: the camera the merge is aligned to (its colour matrix)
	int white; // SAVE_MERGED: the mosaic's white level
	float range; // SAVE_MERGED: an HDR merge's white over the metered exposure's white, else 1
	time_t when; // capture time (saver_queue fills it in when 0)
} SaveJob;

// Writes all files of a job. False when one of them failed. The extension in job->path changes meanwhile.
bool photo_writer_write(SaveJob *job);
