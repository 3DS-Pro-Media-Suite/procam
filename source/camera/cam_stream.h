// The entry point of the camera library, a small wrapper around libctru's camera service (cam:u).
//
// It owns the whole capture pipeline: camera selection, contexts, trimming, transfer, buffered frame delivery
// and error recovery. Every control the service offers is a field of one settings struct, CamSettings. It
// needs only libctru, math_util.h, time_units.h and thread_defaults.h.
//
// Typical use: cam_stream_init, then cam_settings_defaults and cam_stream_start. Call cam_stream_poll every
// frame, and cam_stream_apply after a settings change. cam_stream_exit at the end.
#pragma once
#include <3ds.h>
#include "cam_settings.h"

Result cam_stream_init(void);
void cam_stream_exit(void);

// Configure the whole pipeline for `settings` and start streaming. Safe to call again at any time.
Result cam_stream_start(const CamSettings *settings);
// Stop streaming and put the cameras in standby. cam_stream_start or cam_stream_reset_driver starts again.
void cam_stream_stop(void);

// Apply `settings`. Image controls change live. A new camera or format restarts the stream. In dual, so does
// a manual time the dual frame does not fit, and a burst that found the sensors out of step.
Result cam_stream_apply(const CamSettings *settings);

// Non-blocking. True when a new frame arrived: width x height pixels, top row first, in the format
// cam_stream_format reports. A capture thread fills the frames and restarts the stream by itself after stalls
// and buffer errors. The frame belongs to the caller, who may convert it in place, until the next call that
// returns true. In dual: the view camera's frames. Burst frames never come through here.
bool cam_stream_poll(u16 **pixels, int *width, int *height);

// Recover a wedged sensor: reset the camera driver and start again. Camera thread only.
Result cam_stream_reset_driver(void);
// Frames delivered since the last start (dual: the view sensor's).
u32 cam_stream_frame_count(void);
// True once after a single stream kept restarting. The app should then call cam_stream_reset_driver from the
// camera thread.
bool cam_stream_restart_storm(void);

// Stream raw sensor data from the next cam_stream_start on. Needs the i2c::CAM session.
void cam_stream_set_raw_mode(bool enabled);
// True when the stream is raw 10-bit Bayer data (CAM_FORMAT_RAW), false for YUYV.
bool cam_stream_raw_active(void);

// Diagnostics for the info page.
typedef struct {
	u32 activated; // camera select of the running stream (0: none)
	bool haveCalibration;
	CAMU_StereoCameraCalibrationData calibration;
	float fps; // frames delivered per second
	u32 frames; // frames delivered since cam_stream_start
	u32 restarts; // stalls and buffer errors recovered from
	Result lastError; // first failing call of the last configure or apply
	const char *lastErrorCall;
} CamStreamInfo;

// Makes no camera service call, so the main thread can call it without waiting.
void cam_stream_info(CamStreamInfo *info);
float cam_stream_fps(void);
// The format of the frames cam_stream_poll delivers (CAM_FORMAT_*). Cheap and safe every frame.
int cam_stream_format(void);
