// Bursts and HDR pairs taken straight from the running dual stream.
#pragma once
#include <3ds.h>
#include "still_capture.h"

// dual_burst_take: no raw stream runs. dual_burst_take_hdr: no dual stream runs.
#define DUAL_BURST_NO_STREAM ((Result)0xC8A0FFF0)
// Camera A got at least two frames, but not all: merge what arrived. HDR: the pairs in pairMask.
#define DUAL_BURST_PARTIAL ((Result)0xC8A0FFF1)
// A sensor's own AE / AWB came back on: the frames are not usable.
#define DUAL_BURST_SENSOR_AE ((Result)0xC8A0FFF2)
// A 3D pair or the HDR pairs are not all from one moment, even when claimed again. pairMask says which are.
#define DUAL_BURST_UNPAIRED ((Result)0xC8A0FFF3)
// Camera A got fewer than two frames in time. The same code as the other captures' timeouts (0xFFFFFFFD).
#define DUAL_BURST_TIMEOUT ((Result)(-3))

// Takes frameCount frames per camera (at most CAM_BURST_MAX_FRAMES) from the running raw stream into 640x480
// raw buffers. outA gets camera A (OUT1, the right eye), outB camera B (OUT2, the left eye). With outB NULL
// or a single stream, camera A alone; a single stream must be at 640x480 without the fast view. A 3D pair
// (one frame with outB) is claimed again until both frames are from one moment. The buffers are never written
// after it returns. Fills info. Runs on the camera thread, or on main while the camera thread is idle.
Result dual_burst_take(int frameCount, u8 *const *outA, u8 *const *outB, RawShotInfo *info, u32 timeoutMs);

// HDR pairs: dual_burst_take on the dual stream (outB required), with camera A at linesA (the longer time,
// for the shadows) and camera B at linesB (for the highlights), both at the stream's gain. Each time is
// capped at the longest shutter. The live exposure goes back on afterwards. timeoutMs is raised to what the
// burst needs. An I2C failure writing the times returns that error and no frames.
Result dual_burst_take_hdr(int frameCount, u8 *const *outA, u8 *const *outB, RawShotInfo *info, u32 timeoutMs,
                           u16 linesA, u16 linesB);
