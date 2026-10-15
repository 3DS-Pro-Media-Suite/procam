#include "cam_state.h"
#include <3ds.h>
#include "cam_settings.h"
#include "time_units.h"

// A frame-done older than this many frame periods is no reference for the frame timing.
#define MID_FRAME_MAX_AGE_PERIODS 8

// A port counts as stalled after two frames plus a margin, counted in steps of STALL_STEP_MS.
#define STALL_STEP_MS 100
#define STALL_MARGIN_STEPS 15
#define STALL_STEPS_PER_FRAME_SECOND 20.0f

const u32 CAM_SERVICE_SELECT[CAM_COUNT] = {
        [CAM_OUTER_RIGHT] = SELECT_OUT1,
        [CAM_INNER] = SELECT_IN1,
        [CAM_OUTER_LEFT] = SELECT_OUT2,
};
const u32 CAM_SERVICE_PORT[CAM_COUNT] = {
        [CAM_OUTER_RIGHT] = PORT_CAM1,
        [CAM_INNER] = PORT_CAM1,
        [CAM_OUTER_LEFT] = PORT_CAM2,
};

CamState g_camera;

CamPort *cam_state_view_port(void)
{
	return &g_camera.ports[g_camera.dual ? g_camera.view : CAM_PORT_A];
}

u16 cam_state_frame_lines(void)
{
	return g_camera.hdrFrameLines ? g_camera.hdrFrameLines : g_camera.dualFrameLines;
}

void cam_state_sleep_ticks(u64 ticks)
{
	double nanoseconds = (double)ticks * (double)NANOSECONDS_PER_MILLISECOND / CPU_TICKS_PER_MSEC;
	svcSleepThread((s64)nanoseconds);
}

// Two dual sensors are aligned once, at the start. A sensor that runs a single frame of another length stays
// that far off the other one for good. A sensor takes a timing write at a frame boundary, so the writes go in
// the middle of a frame: outside 0.2 .. 0.75 of the frame, wait until a quarter frame after a frame-done.
void cam_state_wait_mid_frame(u64 lastFrameTick, u64 periodTicks)
{
	u64 now = svcGetSystemTick();
	if (!lastFrameTick || !periodTicks || now < lastFrameTick) {
		return;
	}
	if (now - lastFrameTick > MID_FRAME_MAX_AGE_PERIODS * periodTicks) {
		return;
	}
	u64 phase = (now - lastFrameTick) % periodTicks;
	u64 safeStart = periodTicks / 5;
	u64 safeEnd = periodTicks * 3 / 4;
	if (phase >= safeStart && phase <= safeEnd) {
		return;
	}
	u64 target = periodTicks / 4;
	if (phase >= safeStart) {
		target += periodTicks; // a quarter into the next frame
	}
	cam_state_sleep_ticks(target - phase);
}

u32 cam_state_stall_limit_ms(float frameSeconds)
{
	u32 frameSteps = (u32)(frameSeconds * STALL_STEPS_PER_FRAME_SECOND);
	return STALL_STEP_MS * (STALL_MARGIN_STEPS + frameSteps);
}
