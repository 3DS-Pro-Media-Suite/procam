// Multi-frame raw merge. Many short raw frames from one or both outer cameras go in. One cleaner raw mosaic
// comes out, on camera A's RGGB grid, with the values scaled by MERGE_SCALE. Portable C11, no 3DS calls.
//
// Method:
// 1. The frames are unpacked to u16 in place, and each camera's mapped defects are fixed.
// 2. The reference is the sharpest of camera A's first frames.
// 3. Every other frame is aligned to the reference per tile, to 1/8 of a plane pixel. Camera A's frames
//    follow the hand shake, snapped to an affine model. Camera B's frames are found in a stereo band.
// 4. Camera B's levels are mapped to camera A's per Bayer plane, as a gain and an offset.
// 5. Each Bayer plane is merged on overlapping tiles as a robust weighted mean against the reference. A
//    sample loses its weight where it differs from the reference by more than the noise model allows.
//    Clipped samples never enter, and clipped reference pixels stay as they are.
//
// HDR (MergeJob.hdr): camera A takes the long exposure and is the reference. Camera B takes a shorter one.
// The output's white is where camera B's clip lands at its fitted levels, so the range grows by about
// hdrRatio x hdrSensitivityB. Each pixel has two estimates. Camera A's estimate merges its frames and camera
// B's, weighted by each camera's noise. Camera B's estimate uses only the B frames that agree with camera A
// where A is not clipped. Camera A's share falls to zero as the reference nears white. A clipped position
// without a usable B sample takes the overlapping tiles' values, or else white in all four planes. The stereo
// pair cannot recover what camera A cannot see or check: occlusions, view-dependent highlights and flare end
// at white or at camera A's clip.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MERGE_WIDTH 640
#define MERGE_HEIGHT 480
#define MERGE_CFA_CELLS 4 // R, Gr, Gb, B
#define MERGE_MAX_FRAMES 12 // per camera
#define MERGE_SCALE 4 // the output is the merged raw value times this
// Merge tiles are MERGE_TILE_SIZE plane pixels square and MERGE_TILE_STRIDE apart, so neighbours overlap by
// half. A Bayer plane is half the frame each way.
#define MERGE_TILE_SIZE 16
#define MERGE_TILE_STRIDE 8
#define MERGE_TILE_ROWS ((MERGE_HEIGHT / 2 - MERGE_TILE_SIZE) / MERGE_TILE_STRIDE + 1)
#define MERGE_TILE_COLUMNS ((MERGE_WIDTH / 2 - MERGE_TILE_SIZE) / MERGE_TILE_STRIDE + 1)
#define MERGE_MAX_THREADS 4 // the caller included
#define MERGE_HDR_SENSITIVITY \
	1.23f // camera B's sensitivity against camera A's when MergeJob.hdrSensitivityB is 0

typedef struct {
	// Camera A is the right lens, or the camera in use. Frames are MERGE_WIDTH x MERGE_HEIGHT RGGB.
	uint8_t *framesA[MERGE_MAX_FRAMES];
	int frameCountA;
	// Camera B is the left lens. Without B frames, camera A is merged alone.
	uint8_t *framesB[MERGE_MAX_FRAMES];
	int frameCountB;
	bool packed; // frames hold the camera's 8+2 bit stream (unpacked in place); else u16 little-endian
	float blackA[MERGE_CFA_CELLS];
	float blackB[MERGE_CFA_CELLS];
	int gain; // analog gain code of the shot (8 = x1), for the noise model
	// Defect maps: x, y pairs in full-frame coordinates, or NULL.
	const uint16_t *hotA;
	const uint16_t *hotB;
	int hotCountA;
	int hotCountB;
	_Atomic int *progress; // 0..100, may be NULL
	char *log; // the processing log is appended here, may be NULL
	size_t logCapacity;
	uint64_t (*clockMicroseconds)(void); // for the stage times in the log, may be NULL
	// Worker threads are optional: with spawn NULL or threads <= 1, everything runs on the caller. The work
	// is handed out in small pieces, so the result is the same bit for bit for any thread count. spawn starts
	// fn(arg) as worker 1..threads-1 and returns a handle, or NULL when it cannot. join waits for the thread
	// and frees it. A worker needs under 8 KB of stack. Each extra thread needs about 0.9 MB of working
	// memory; when that does not fit, the merge runs on fewer threads.
	int threads; // 1..MERGE_MAX_THREADS, the caller included
	void *(*spawn)(void (*fn)(void *arg), void *arg, int worker);
	void (*join)(void *handle);
	// HDR needs B frames, else camera A is merged alone. hdrRatio is camera A's exposure over camera B's,
	// (linesA x gainA) / (linesB x gainB), at least 1. gain is then camera A's gain code and gainB camera B's
	// (0: the same as gain). B_k is taken at the same instant as A_k. hdrSensitivityB is camera B's own
	// sensitivity per CFA cell at equal exposure: A - blackA = hdrSensitivityB x (B - blackB), as
	// MergeResult.levelGain measures it in a plain merge (0: MERGE_HDR_SENSITIVITY). Camera B's gain is then
	// hdrRatio x hdrSensitivityB, and only its offset is fitted, because a fitted slope comes out too flat
	// over noisy dark tiles. Camera B is dropped when the data's own level ratio is far off that gain.
	bool hdr;
	float hdrRatio;
	float hdrSensitivityB[MERGE_CFA_CELLS];
	int gainB;
} MergeJob;

typedef struct {
	uint16_t *mosaic; // MERGE_WIDTH x MERGE_HEIGHT RGGB, the merged value x MERGE_SCALE; the caller frees it
	int referenceIndex; // the camera A frame used as the reference
	float useA; // mean merge weight of camera A's other frames over unclipped tiles, 0..1
	float useB; // the same for camera B's frames
	// Camera B to A per CFA cell: reference - blackA = levelGain x (B - blackB) + levelOffset.
	float levelGain[MERGE_CFA_CELLS];
	float levelOffset[MERGE_CFA_CELLS];
	int fixedDefects; // over all frames
	uint32_t alignMs; // stage times, when there is a clock
	uint32_t mergeMs;
	int threads; // threads that took part, the caller included
	// The mosaic's white level: MERGE_SCALE x 1023, or for HDR where camera B's clip lands at its levels
	// (at most 65535). The black level is MERGE_SCALE x the input black.
	int white;
	float hdrRatio; // white over MERGE_SCALE x 1023; 1 without an HDR merge
	float hdrAbove; // HDR: share of samples above camera A's clip
	float hdrWhite; // HDR: share of samples left at white, where no camera B sample could be used
	bool hdrUsed; // an HDR merge with camera B in it; equal exposures still merge as HDR
} MergeResult;

// Runs the whole merge. The frames are modified: unpacked, defects fixed. Working memory is about 2.5 MB
// besides the frames, plus 0.9 MB per extra thread; HDR adds 88 KB per thread and 0.15 MB. Returns false
// when out of memory (result->mosaic is then NULL) or when a frame count is out of range.
bool merge_run(MergeJob *job, MergeResult *result);
