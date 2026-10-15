// The dual raw stream: both outer sensors run raw with one frame timing, vsync-aligned once at the start.
// Only the view camera's port streams, at full frames. dual_burst_take starts the other port for a shot
// only, so the shutter needs no sensor set-up and a pair is one moment.
#pragma once
#include <3ds.h>
#include "sensor_exposure.h"

#define DUAL_FRAME_LINES 560 // full frame length in dual: its blanking leaves time to re-arm both ports
#define DUAL_MAX_EXPOSURE_LINES (DUAL_FRAME_LINES - SENSOR_EXPOSURE_MARGIN_LINES) // no stretched frame

// Dual from the next cam_stream_start while raw and an outer camera are chosen. false also clears the
// given-up state (dual_stream_broken) and its history, so choosing the mode again retries.
void dual_stream_set_wanted(bool on);
bool dual_stream_active(void); // the running stream is a dual one
// The dual stream kept restarting or its bursts kept failing: the next cam_stream_apply drops to one camera.
bool dual_stream_broken(void);
// A burst found the sensors out of step: the next cam_stream_apply restarts the stream to align them.
bool dual_stream_desync(void);
