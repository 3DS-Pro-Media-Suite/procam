// The dual stream's part of the camera library: its start, its end and its frame length.
#pragma once
#include <3ds.h>
#include "cam_settings.h"

// The full frame length both sensors run for these settings: DUAL_FRAME_LINES, or longer for a manual time
// that does not fit in it.
u16 dual_stream_frame_lines(const CamSettings *settings);
// Frees camera B's ring, its error event and the sink. The stream must be paused.
void dual_stream_free(void);
// Leaves the dual stream: camera B back to YUYV, both sensors' own frame lengths back. The stream must be
// stopped, the sensors still active.
void dual_stream_leave(void);
// At the app's exit: both outer sensors back to the module's own AE and AWB, then dual_stream_leave.
void dual_stream_exit(void);
// cam_stream_start's dual branch. false: dual is left again and the caller starts one camera.
bool dual_stream_start(const CamSettings *settings, Result *firstError);
