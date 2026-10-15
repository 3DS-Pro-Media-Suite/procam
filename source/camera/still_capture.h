// Captures that set the sensors up for one shot: the YUV still, the YUV 3D pair and the slow raw burst.
#pragma once
#include <3ds.h>
#include "cam_settings.h"

// For a YUV stream only (no raw sensor access). A raw stream's photo is its next frame.
// Blocking VGA still in the stream's format into `destination`, CAM_FRAME_BYTES bytes. Switches to context B,
// whose settings are the stream's but for the size, drops `skipFrames` frames while the sensor settles, then
// restarts the preview.
Result still_capture_yuv_photo(u16 *destination, int skipFrames);

// Blocking 3D pair from the YUV stream: one VGA YUYV frame from each outer camera at the same moment, with
// synchronised frames and brightness. outA gets OUT1 (the right eye), outB OUT2 (the left eye), each
// CAM_FRAME_BYTES bytes. Restarts the preview after.
Result still_capture_yuv_pair(const CamSettings *settings, u8 *outA, u8 *outB);

// Slots of RawShotInfo.timing, in the order of SENSOR_TIMING_REGISTERS. The read mode's mirroring decides the
// Bayer order.
enum {
	RAW_SHOT_READ_MODE,
	RAW_SHOT_FRAME_LENGTH,
	RAW_SHOT_LINE_LENGTH,
	RAW_SHOT_COARSE_TIME,
	RAW_SHOT_FINE_TIME,
	RAW_SHOT_TIMING_COUNT
};

// The cameras of a raw or dual burst, the index of every array of RawShotInfo. Camera A is OUT1 (the right
// eye) or the inner camera, camera B is OUT2 (the left eye).
#define RAW_SHOT_CAMERA_COUNT 2
#define RAW_SHOT_CAMERA_A 0
#define RAW_SHOT_CAMERA_B 1

// What a raw or dual burst reports.
typedef struct {
	u16 lines; // integration lines written to the sensors (camera A's in an HDR burst)
	u16 gain; // analog gain code written to both sensors
	u16 sequencerMode[RAW_SHOT_CAMERA_COUNT]; // SEQ_MODE at the end; 0x0A: the sensors' AE and AWB stayed off
	u16 linesBack[RAW_SHOT_CAMERA_COUNT]; // COARSE_INTEGRATION_TIME read back at the end
	int received
	        [RAW_SHOT_CAMERA_COUNT]; // frames that arrived; after a timeout this shows which camera stalled
	bool bufferError[RAW_SHOT_CAMERA_COUNT]; // the port's buffer error event fired (overrun)
	// Sensor timing registers; they must match between the sensors.
	u16 timing[RAW_SHOT_CAMERA_COUNT][RAW_SHOT_TIMING_COUNT];
	float skewMs; // mean gap between the two cameras' frame-done signals
	float skewMaxMs; // largest gap
	// Integration lines of each camera's frames; they differ only in an HDR burst.
	u16 cameraLines[RAW_SHOT_CAMERA_COUNT];
	u32 pairMask; // bit i: pair i (outA[i], outB[i]) finished within a quarter frame, so it shows one moment
	u8 paired; // number of bits set in pairMask
	u8 reclaims; // times dual_burst_take claimed its frames again because the pairs did not match
} RawShotInfo;

// Blocking raw burst; the stream must be raw. `frames` VGA raw frames (8+2 bits, RGGB) from camera A (OUT1,
// the right eye, or the inner camera) and, with `stereo`, from camera B (OUT2, the left eye) at the same
// moments. Both sensors get the stream's exposure, so no HDR. Slow: it sets both sensors up and back and
// leaves a dual stream for the time, so the dual stream's own bursts are the usual path. `info` may be NULL.
Result still_capture_raw_burst(const CamSettings *settings, int frames, bool stereo, u8 *const *outA,
                               u8 *const *outB, RawShotInfo *info);

// The next raw capture plays the shutter sound once its frames are in.
void still_capture_set_late_sound(bool enabled);

// True once when the shutter sound is due; taking it clears it.
bool still_capture_take_late_sound(void);
