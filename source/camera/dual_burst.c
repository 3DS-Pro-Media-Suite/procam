// Bursts taken from the running stream: the capture thread receives the frames straight into the caller's
// buffers.
#include "dual_burst.h"
#include <3ds.h>
#include <string.h>
#include "cam_settings.h"
#include "cam_state.h"
#include "capture_thread.h"
#include "fast_view.h"
#include "sensor_exposure.h"
#include "sensor_i2c.h"
#include "still_capture.h"
#include "time_units.h"

#define TICKS_PER_MICROSECOND (CPU_TICKS_PER_MSEC / (float)MICROSECONDS_PER_MILLISECOND)

#define POLL_MS 2
#define FRAME_EDGE_POLL_MS 1 // finer polling where the end of a frame matters

#define UNVERIFIED_WAIT_MAX_MS 600
#define SETTLE_WAIT_MAX_MS 300
// A write can land a frame apart on the two sensors, and the second frame after it is the first entirely at
// the new time.
#define EXPOSURE_SETTLE_FRAMES 3

#define BURST_FAIL_LIMIT 3 // dual bursts failed in a row before the dual stream is given up
#define PARTIAL_MIN_FRAMES 2 // camera A frames a partial burst needs: fewer is a timeout
#define MAX_CLAIMS 2
#define MAX_CLAIMS_PAIRED 3 // 3D and HDR claim again until the pairs are from one moment
#define CLAIM_AHEAD_FRAMES 2 // the receive already armed is for the next frame: claim from the one after
#define PAIR_OFFSET_VALID_MS 1000 // how long a pair offset from the capture thread is trusted

#define LIVE_FRAME_WAIT_MARGIN_MS 100
// The other port's start must fall well inside a live frame for its frame numbers to be predicted.
#define START_PHASE_MIN 0.15
#define START_PHASE_MAX 0.95
#define OTHER_FIRST_FRAME 2 // a port's first frame after a start may be cut short: the burst skips it
#define PAIR_WAIT_FRAMES 4
#define PAIR_WAIT_MARGIN_MS 50
#define STOP_WAIT_MARGIN_MS 200
#define RELEASE_MARGIN_MS 50
#define RESTART_WAIT_MS 600 // the capture thread lets go of a receive soon after a restart is asked for

#define HDR_TIMEOUT_EXTRA_FRAMES 8 // frames an HDR burst may wait for before its own
#define HDR_TIMEOUT_MARGIN_MS 1500
#define HIDE_UNTIL_RESTORE 0x10000 // frames: the live view hides the HDR frames until the restore
#define HIDE_AFTER_RESTORE_FRAMES 2 // the second frame after a write is the first entirely at the live time

static const u8 DUAL_SENSOR_DEVICES[CAM_PORT_COUNT] = {SENSOR_DEVICE_OUT1, SENSOR_DEVICE_OUT2};

// An HDR burst's two integration times, from hdr_write until hdr_restore.
typedef struct {
	bool requested;
	bool applied; // the times are on the sensors
	u16 lines[CAM_PORT_COUNT]; // camera A's and B's integration lines
	u16 frameLines; // both sensors' frame length meanwhile: the dual frame, or longer when a time needs it
	u16 liveGain; // the live gain when the times went on
	u32 liveStallMs; // the stall limit before
	Result writeResult;
} HdrRun;

// What ready_begin set up for a dual burst, for ready_end to undo.
typedef struct {
	bool begun;
	u32 extraPorts; // ports started for the burst beside the live one (a port mask)
	bool predicted; // firstFrame follows from how the other port started; else the pairs seen decide
	u32 firstFrame[CAM_PORT_COUNT];
} ReadyRun;

// How the other port's StartCapture fell against the live port's frames.
typedef struct {
	u32 liveFrame; // the live frame that ended last before it
	u32 liveRestarts; // the live port's restarts before it
	u64 tick; // svcGetSystemTick at the StartCapture
	u64 ms; // osGetTime at the StartCapture
} PortStart;

// One burst: the request, its set-up and what came back.
typedef struct {
	int frameCount; // frames per camera claimed, at most CAM_BURST_MAX_FRAMES
	int requestedCount; // frames per camera the caller asked for
	u8 *const *outA;
	u8 *const *outB;
	RawShotInfo *info;
	bool stereo; // camera B takes part
	bool needPairs; // every pair must be from one moment: 3D and HDR
	HdrRun hdr;
	ReadyRun ready;
	u64 startMs;
	u32 bufferErrorsAtStart[CAM_PORT_COUNT];
	u32 restartsAtStart[CAM_PORT_COUNT];
	u64 claimMs; // when the last claim started
	u64 doneMs; // when the last claim ended
	int received[CAM_PORT_COUNT];
	bool lost[CAM_PORT_COUNT];
	int reclaims;
} BurstRun;

static void sleep_ms(s64 milliseconds)
{
	svcSleepThread(milliseconds * NANOSECONDS_PER_MILLISECOND);
}

static u64 tick_gap(u64 tickA, u64 tickB)
{
	return tickA > tickB ? tickA - tickB : tickB - tickA;
}

// The single stream's frame length: shorter in the fast view.
static int single_frame_lines(void)
{
	return g_camera.fastViewOn ? SENSOR_FULL_FRAME_LINES - FAST_VIEW_FRAME_CUT : SENSOR_FULL_FRAME_LINES;
}

// A port's frame time in ms: its period, or the nominal one before its first frames.
static double frame_ms(const CamPort *port)
{
	// Read once: the capture thread may reset it in between.
	u64 periodTicks = port->periodTicks;
	if (periodTicks) {
		return (double)periodTicks / CPU_TICKS_PER_MSEC;
	}
	int frameLines = g_camera.dual ? cam_state_frame_lines() : single_frame_lines();
	double frameMicroseconds = frameLines * sensor_exposure_line_microseconds();
	return frameMicroseconds / (float)MICROSECONDS_PER_MILLISECOND;
}

// The capture thread did not let go of a receive aimed at a burst buffer or the sink. Stopping every port
// cancels the receives; then the steady ports start again with their rings as they are.
static void ports_reset_steady(void)
{
	// The thread forgets its receives, and StopCapture cancels them.
	capture_thread_pause();
	CAMU_StopCapture(PORT_BOTH);
	LightLock_Lock(&g_camera.lock);
	g_camera.streamingPorts = g_camera.dual ? 1u << g_camera.view : CAM_PORT_MASK_A;
	g_camera.ports[CAM_PORT_B].stopAfterBurst = false;
	g_camera.ports[CAM_PORT_A].stopAfterBurst = false;
	LightLock_Unlock(&g_camera.lock);
	CAMU_ClearBuffer(g_camera.livePort);
	CAMU_StartCapture(g_camera.livePort);
	u64 now = osGetTime();
	for (int port = 0; port < CAM_PORT_COUNT; port++) {
		g_camera.ports[port].lastFrameMs = now;
	}
	g_camera.unverifiedUntil = now + CAM_UNVERIFIED_AFTER_START_MS;
	sensor_exposure_ask_guard();
	g_camera.streamWanted = true;
	if (g_camera.wakeEvent) {
		svcSignalEvent(g_camera.wakeEvent);
	}
}

// Once a burst returns, no receive may still be aimed at the caller's buffers: they get freed. A frame on its
// way lands within a frame time. A port that stalls is restarted, as its restart cancels the receive.
static void burst_release(u64 framePeriodMs)
{
	u64 startMs = osGetTime();
	u64 restartAfterMs = 2 * framePeriodMs + RELEASE_MARGIN_MS;
	bool restartAsked = false;
	for (;;) {
		LightLock_Lock(&g_camera.lock);
		bool pending[CAM_PORT_COUNT] = {g_camera.ports[CAM_PORT_A].armedSlot >= 0,
		                                g_camera.ports[CAM_PORT_B].armedSlot >= 0};
		LightLock_Unlock(&g_camera.lock);
		if (!pending[CAM_PORT_A] && !pending[CAM_PORT_B]) {
			return;
		}
		u64 elapsedMs = osGetTime() - startMs;
		if (!restartAsked && elapsedMs > restartAfterMs) {
			for (int port = 0; port < CAM_PORT_COUNT; port++) {
				if (pending[port]) {
					g_camera.ports[port].restartForBurst = true;
				}
			}
			restartAsked = true;
		} else if (elapsedMs > restartAfterMs + RESTART_WAIT_MS) {
			break;
		}
		sleep_ms(POLL_MS);
	}
	ports_reset_steady();
}

// The longest HDR time is the longest shutter setting: longer exposures hang the camera.
static u16 hdr_max_lines(void)
{
	float longestSeconds = CAM_SHUTTER_SECONDS[CAM_SHUTTER_COUNT - 1];
	float lines = longestSeconds * (float)MICROSECONDS_PER_SECOND / sensor_exposure_line_microseconds();
	if (lines > SENSOR_LINES_MAX) {
		return SENSOR_LINES_MAX;
	}
	if (lines < 1) {
		return 1;
	}
	return (u16)lines;
}

// Caps both HDR times and picks one frame length for both sensors that covers the longer time, since dual
// frames never stretch. Raises the timeout to what the frames need at that length.
static void hdr_prepare(HdrRun *hdr, const u16 *hdrLines, int frameCount, u32 *timeoutMs)
{
	u16 longestLines = 0;
	u16 maxLines = hdr_max_lines();
	for (int port = 0; port < CAM_PORT_COUNT; port++) {
		// 0 lines is a sub-line time: the fine part alone.
		hdr->lines[port] = hdrLines[port] > maxLines ? maxLines : hdrLines[port];
		if (hdr->lines[port] > longestLines) {
			longestLines = hdr->lines[port];
		}
	}
	hdr->requested = true;
	int coveringLines = longestLines + SENSOR_EXPOSURE_MARGIN_LINES;
	if (coveringLines > g_camera.dualFrameLines) {
		hdr->frameLines = (u16)coveringLines;
	} else {
		hdr->frameLines = g_camera.dualFrameLines;
	}
	int timeoutLines = (frameCount + HDR_TIMEOUT_EXTRA_FRAMES) * hdr->frameLines;
	float timeoutMicroseconds = timeoutLines * sensor_exposure_line_microseconds();
	u32 neededMs = (u32)(timeoutMicroseconds / (float)MICROSECONDS_PER_MILLISECOND) + HDR_TIMEOUT_MARGIN_MS;
	if (*timeoutMs < neededMs) {
		*timeoutMs = neededMs;
	}
}

// Writes one register on both dual sensors: valueA on camera A's, valueB on camera B's. Returns the first
// failure.
static Result write_sensors(u16 reg, u16 valueA, u16 valueB)
{
	const u16 values[CAM_PORT_COUNT] = {valueA, valueB};
	Result result = 0;
	for (int device = 0; device < CAM_PORT_COUNT; device++) {
		Result written = sensor_i2c_write16(DUAL_SENSOR_DEVICES[device], reg, values[device]);
		if (R_FAILED(written) && R_SUCCEEDED(result)) {
			result = written;
		}
	}
	return result;
}

// The capture thread's stall limit for frames of this length.
static u32 stall_limit_ms(u16 frameLines)
{
	float frameSeconds = frameLines * sensor_exposure_line_microseconds() / (float)MICROSECONDS_PER_SECOND;
	return cam_state_stall_limit_ms(frameSeconds);
}

// Writes the HDR times on both sensors back to back, in the middle of a frame, so both take them on the same
// frame. A longer frame length goes first: an exposure longer than its frame would stretch that frame on one
// sensor alone. Direct registers only: another thread may be in the middle of a two-step MCU access.
static Result hdr_write(HdrRun *hdr)
{
	CamPort *viewPort = cam_state_view_port();
	// From here sensor_exposure_set_lines and sensor_exposure_set_gain only note the live exposure.
	g_camera.hdrOn = true;
	hdr->liveGain = g_camera.exposureGain;
	hdr->liveStallMs = g_camera.stallMs;
	Result result = 0;
	bool longerFrame = hdr->frameLines != g_camera.dualFrameLines;
	if (longerFrame) {
		result = write_sensors(SENSOR_REG_FRAME_LENGTH, hdr->frameLines, hdr->frameLines);
		if (R_FAILED(result)) {
			// One sensor's frame may be longer now.
			g_camera.dualDesync = true;
		}
	}
	Result timesWritten =
	        write_sensors(SENSOR_REG_COARSE_TIME, hdr->lines[CAM_PORT_A], hdr->lines[CAM_PORT_B]);
	if (R_SUCCEEDED(result)) {
		result = timesWritten;
	}
	hdr->applied = true;
	hdr->writeResult = result;
	g_camera.hdrFrameLines = hdr->frameLines;
	if (longerFrame) {
		for (int port = 0; port < CAM_PORT_COUNT; port++) {
			g_camera.ports[port].resetPeriodAverage = true;
		}
		u32 stallMs = stall_limit_ms(hdr->frameLines);
		if (stallMs > g_camera.stallMs) {
			g_camera.stallMs = stallMs;
		}
	}
	// The burst claims frames from the third one after this. The live view hides them until the restore.
	g_camera.exposureWriteFrame = viewPort->framesDone;
	g_camera.hideFramesUntil = viewPort->framesDone + HIDE_UNTIL_RESTORE;
	return result;
}

// Puts the live exposure back on both sensors after an HDR burst, in the middle of a frame of the HDR length.
// The times go first: the other way round, a long time would stretch a shorter frame on one sensor alone.
static void hdr_restore(HdrRun *hdr)
{
	if (!hdr->applied) {
		return;
	}
	CamPort *viewPort = cam_state_view_port();
	u64 lastTick = viewPort->lastTick;
	float frameMicroseconds = hdr->frameLines * sensor_exposure_line_microseconds();
	u64 framePeriodTicks = (u64)(frameMicroseconds * TICKS_PER_MICROSECOND);
	cam_state_wait_mid_frame(lastTick, framePeriodTicks);
	Result timesWritten =
	        write_sensors(SENSOR_REG_COARSE_TIME, g_camera.exposureLines, g_camera.exposureLines);
	bool longerFrame = hdr->frameLines != g_camera.dualFrameLines;
	if (longerFrame) {
		Result frameWritten =
		        write_sensors(SENSOR_REG_FRAME_LENGTH, g_camera.dualFrameLines, g_camera.dualFrameLines);
		if (R_FAILED(frameWritten) && R_SUCCEEDED(timesWritten)) {
			g_camera.dualDesync = true;
		}
	}
	if (g_camera.exposureGain != hdr->liveGain) {
		// Every gain register, whatever the others did: a failed write must not leave the colours unmatched.
		for (int device = 0; device < CAM_PORT_COUNT; device++) {
			for (u16 reg = SENSOR_REG_GAIN_GLOBAL; reg <= SENSOR_REG_GAIN_LAST; reg += SENSOR_REG_GAIN_STEP) {
				sensor_i2c_write16(DUAL_SENSOR_DEVICES[device], reg, g_camera.exposureGain);
			}
		}
	}
	hdr->applied = false;
	g_camera.hdrFrameLines = 0;
	g_camera.hdrOn = false;
	if (longerFrame) {
		for (int port = 0; port < CAM_PORT_COUNT; port++) {
			g_camera.ports[port].resetPeriodAverage = true;
		}
		g_camera.stallMs = hdr->liveStallMs;
		// The line time is averaged over frames that still include the longer ones.
		g_camera.lineHoldUntil = osGetTime() + CAM_LINE_HOLD_MS;
	}
	g_camera.exposureWriteFrame = viewPort->framesDone;
	g_camera.hideFramesUntil = viewPort->framesDone + HIDE_AFTER_RESTORE_FRAMES;
}

// Waits for the live port's frame after frameBefore, then for the middle of the next one. Returns the number
// of the live frame that ended last.
static u32 wait_mid_live_frame(const CamPort *live, u32 frameBefore, double frameMs)
{
	u64 waitEndMs = osGetTime() + (u64)(2 * frameMs) + LIVE_FRAME_WAIT_MARGIN_MS;
	while (live->framesDone == frameBefore && osGetTime() < waitEndMs) {
		sleep_ms(FRAME_EDGE_POLL_MS);
	}
	// The capture thread finishes that frame's bookkeeping (lastTick).
	sleep_ms(FRAME_EDGE_POLL_MS);
	u32 liveFrame = live->framesDone;
	cam_state_wait_mid_frame(live->lastTick, live->periodTicks);
	return liveFrame;
}

// Where a tick lies in the live port's current frame, as a fraction of the frame; -1 when unknown.
static double live_frame_phase(const CamPort *live, u64 tick)
{
	u64 lastLiveTick = live->lastTick;
	u64 livePeriodTicks = live->periodTicks;
	bool phaseKnown = livePeriodTicks && tick > lastLiveTick;
	return phaseKnown ? (double)(tick - lastLiveTick) / (double)livePeriodTicks : -1;
}

// The other port starts at the other sensor's next frame start, the one of the live port's frame
// liveFrame + 2, so its frame k is the live port's liveFrame + 1 + k. That holds while the start came inside
// frame liveFrame + 1 and the live port went on undisturbed; otherwise the pairs the capture thread sees
// decide.
static void predict_first_frames(ReadyRun *ready, int liveIndex, const PortStart *start, double frameMs)
{
	const CamPort *live = &g_camera.ports[liveIndex];
	double startPhase = live_frame_phase(live, start->tick);
	bool undisturbed = live->framesDone == start->liveFrame && live->restarts == start->liveRestarts;
	if (undisturbed && startPhase > START_PHASE_MIN && startPhase < START_PHASE_MAX) {
		ready->predicted = true;
		ready->firstFrame[liveIndex] = start->liveFrame + 1 + OTHER_FIRST_FRAME;
		ready->firstFrame[cam_state_other_port(liveIndex)] = OTHER_FIRST_FRAME;
		return;
	}
	u64 pairWaitEndMs = osGetTime() + (u64)(PAIR_WAIT_FRAMES * frameMs) + PAIR_WAIT_MARGIN_MS;
	while (g_camera.pairAtMs < start->ms && osGetTime() < pairWaitEndMs) {
		sleep_ms(POLL_MS);
	}
}

// Starts the other camera's port for a dual burst. Both sensors run frame-locked at full frames all the time;
// only the ports change. The live port keeps streaming. The other port starts in the middle of a frame, right
// after a live frame: the live port is armed for its next frame, and the other sensor's next frame start is
// about 3/4 frame away, time enough to arm its port. An HDR burst's times are written at the same moment.
static Result ready_begin(u32 neededPorts, ReadyRun *ready, HdrRun *hdr)
{
	int liveIndex = g_camera.view;
	CamPort *live = &g_camera.ports[liveIndex];
	ready->begun = true;
	ready->extraPorts = neededPorts & ~(1u << liveIndex);
	// CAM_TRY records its failure here.
	Result err = 0;
	if (!ready->extraPorts) {
		return 0;
	}
	// The dual stream takes the sink at its start.
	if (!g_camera.sink) {
		return CAM_ERROR_NO_SINK;
	}
	int otherIndex = cam_state_other_port(liveIndex);
	double frameMs = frame_ms(live);
	PortStart start;
	u32 frameBefore = live->framesDone;
	start.liveRestarts = live->restarts;
	start.liveFrame = wait_mid_live_frame(live, frameBefore, frameMs);
	if (hdr) {
		// Both sensors take the times on frame liveFrame + 2, the one the other port starts with.
		hdr_write(hdr);
	}
	LightLock_Lock(&g_camera.lock);
	capture_thread_reset_port(&g_camera.ports[otherIndex], osGetTime());
	// The other port's numbering starts again: a pair seen before says nothing about it.
	g_camera.pairAtMs = 0;
	LightLock_Unlock(&g_camera.lock);
	CAM_TRY_AS("CAMU_ClearBuffer(S.portId[O])", CAMU_ClearBuffer(g_camera.portIds[otherIndex]));
	if (g_camera.ports[otherIndex].errorEvent) {
		svcClearEvent(g_camera.ports[otherIndex].errorEvent);
	}
	start.tick = svcGetSystemTick();
	start.ms = osGetTime();
	Result started = CAMU_StartCapture(g_camera.portIds[otherIndex]);
	// A window as after any StartCapture: the guard ready_end asks for looks once it has passed, and the next
	// burst waits it out.
	g_camera.unverifiedUntil = osGetTime() + CAM_UNVERIFIED_AFTER_START_MS;
	if (R_FAILED(started)) {
		return started;
	}
	LightLock_Lock(&g_camera.lock);
	g_camera.streamingPorts |= ready->extraPorts;
	LightLock_Unlock(&g_camera.lock);
	if (g_camera.wakeEvent) {
		// The capture thread arms the port now.
		svcSignalEvent(g_camera.wakeEvent);
	}
	predict_first_frames(ready, liveIndex, &start, frameMs);
	return err;
}

// The streaming ports beside the live one.
static u32 other_streaming_ports(u32 liveBit)
{
	return g_camera.streamingPorts & ~liveBit;
}

// Stops the other camera's port after a dual burst and leaves the live port alone. The capture thread stops
// it right after a live frame, and its StopCapture cancels the receive into the sink. The guard is asked for:
// the module may switch the other sensor's AE / AWB back on after its port's StartCapture.
static void ready_end(ReadyRun *ready)
{
	if (!ready->begun) {
		return;
	}
	int liveIndex = g_camera.view;
	CamPort *live = &g_camera.ports[liveIndex];
	u32 liveBit = 1u << liveIndex;
	u64 startMs = osGetTime();
	if (other_streaming_ports(liveBit)) {
		for (int port = 0; port < CAM_PORT_COUNT; port++) {
			if ((other_streaming_ports(liveBit) >> port) & 1) {
				g_camera.ports[port].stopAfterBurst = true;
			}
		}
		double frameMs = frame_ms(live);
		while (other_streaming_ports(liveBit) &&
		       osGetTime() < startMs + (u64)(2 * frameMs) + STOP_WAIT_MARGIN_MS) {
			sleep_ms(FRAME_EDGE_POLL_MS);
		}
		// The other port did not stop in time.
		if (other_streaming_ports(liveBit)) {
			ports_reset_steady();
		}
	}
	sensor_exposure_ask_guard();
}

// A burst's frame period in ticks: the shortest gap between camera A's consecutive frames, since a restart or
// a lost frame only makes gaps longer. With too few frames, the port's period or the nominal one.
static double burst_period(int framesA)
{
	const u64 *ticksA = g_camera.burst.frameTicks[CAM_PORT_A];
	double shortestGap = 0;
	for (int i = 1; i < framesA && i < CAM_BURST_MAX_FRAMES; i++) {
		u64 previousTick = ticksA[i - 1];
		u64 tick = ticksA[i];
		if (tick > previousTick && (shortestGap == 0 || (double)(tick - previousTick) < shortestGap)) {
			shortestGap = (double)(tick - previousTick);
		}
	}
	if (shortestGap > 0) {
		return shortestGap;
	}
	return frame_ms(&g_camera.ports[CAM_PORT_A]) * CPU_TICKS_PER_MSEC;
}

// After BURST_FAIL_LIMIT failed dual bursts in a row the next cam_stream_apply starts one camera. The count
// runs across restarts, since each failed burst's fallback restarts the stream.
static void burst_failed(void)
{
	g_camera.burstFails++;
	if (g_camera.burstFails >= BURST_FAIL_LIMIT && !g_camera.dualBroken) {
		g_camera.dualBroken = true;
	}
}

// A burst needs a running raw stream of whole frames: the single stream's fast view reads half frames.
static bool stream_has_full_raw_frames(void)
{
	if (!g_camera.initialized || !g_camera.running || !g_camera.rawActive || !g_camera.streamWanted) {
		return false;
	}
	if (g_camera.dual) {
		return true;
	}
	bool fullSize = g_camera.frameWidth == CAM_FRAME_WIDTH && g_camera.frameHeight == CAM_FRAME_HEIGHT;
	return !g_camera.fastViewOn && fullSize;
}

// The module switches a sensor's AE / AWB back on some time after a StartCapture, and a check made before
// that sees nothing. So a burst waits out a (re)start's unverified window first.
static void wait_out_unverified_window(u64 startMs)
{
	while (osGetTime() < g_camera.unverifiedUntil && osGetTime() < startMs + UNVERIFIED_WAIT_MAX_MS) {
		sleep_ms(POLL_MS);
	}
}

static s32 frames_since_exposure_write(void)
{
	return (s32)(cam_state_view_port()->framesDone - g_camera.exposureWriteFrame);
}

// Switches the sensors' own AE / AWB off again where they came back on, then lets the exposure settle.
static void wait_exposure_settled(void)
{
	sensor_exposure_run_guard();
	u64 guardDoneMs = osGetTime();
	while (osGetTime() < guardDoneMs + SETTLE_WAIT_MAX_MS &&
	       frames_since_exposure_write() < EXPOSURE_SETTLE_FRAMES) {
		sleep_ms(POLL_MS);
	}
}

// The first frames neither port has armed a receive for. Camera B's frame is the one that finishes with
// camera A's: pairOffset frames apart in number, or the current gap without a recent pair.
static void next_free_frames(u32 first[CAM_PORT_COUNT], bool stereo)
{
	u32 doneA = g_camera.ports[CAM_PORT_A].framesDone;
	u32 doneB = g_camera.ports[CAM_PORT_B].framesDone;
	s32 offset = 0;
	if (stereo) {
		bool recentPair = g_camera.pairAtMs && osGetTime() < g_camera.pairAtMs + PAIR_OFFSET_VALID_MS;
		offset = recentPair ? g_camera.pairOffset : (s32)(doneB - doneA);
	}
	u32 firstA = doneA;
	if (stereo && (s32)(doneB - (u32)offset - doneA) > 0) {
		firstA = doneB - (u32)offset;
	}
	firstA += CLAIM_AHEAD_FRAMES;
	first[CAM_PORT_A] = firstA;
	first[CAM_PORT_B] = firstA + (u32)offset;
}

// Hands the claim to the capture thread. Called under g_camera.lock.
static void start_claim(const BurstRun *run, const u32 first[CAM_PORT_COUNT], bool lastClaim)
{
	g_camera.burst.first[CAM_PORT_A] = first[CAM_PORT_A];
	g_camera.burst.first[CAM_PORT_B] = first[CAM_PORT_B];
	g_camera.burst.frameCount = run->frameCount;
	g_camera.burst.destinations[CAM_PORT_A] = run->outA;
	g_camera.burst.destinations[CAM_PORT_B] = run->stereo ? run->outB : NULL;
	g_camera.burst.pairs = run->needPairs;
	g_camera.burst.mismatch = false;
	// The other camera's port stops once all frames are in, unless the pairs may have to be claimed again.
	bool stopOtherPort = !run->needPairs || lastClaim;
	g_camera.burst.stopMask = stopOtherPort ? run->ready.extraPorts : 0;
	for (int port = 0; port < CAM_PORT_COUNT; port++) {
		g_camera.burst.received[port] = 0;
		g_camera.burst.lost[port] = false;
	}
	LightEvent_Clear(&g_camera.burst.done);
	g_camera.burst.active = true;
}

// A pair whose two frames finished within the pair window is from one moment.
static bool is_pair_at_one_moment(double gapTicks, double periodTicks)
{
	return gapTicks < periodTicks / CAM_PAIR_WINDOW_DIVISOR;
}

// Pairs among the first frameCount whose two frames finished at one moment.
static int count_pairs_at_one_moment(int frameCount, int framesA)
{
	double periodTicks = burst_period(framesA);
	int paired = 0;
	for (int i = 0; i < frameCount; i++) {
		u64 gap =
		        tick_gap(g_camera.burst.frameTicks[CAM_PORT_A][i], g_camera.burst.frameTicks[CAM_PORT_B][i]);
		if (is_pair_at_one_moment((double)gap, periodTicks)) {
			paired++;
		}
	}
	return paired;
}

// Claims the burst's frames and waits for them. A claim is made again when the capture thread sees a pair off
// its frame numbers, and for 3D and HDR while the pairs are not from one moment.
static void claim_frames(BurstRun *run, u32 timeoutMs)
{
	int frameCount = run->frameCount;
	int viewIndex = g_camera.dual ? g_camera.view : CAM_PORT_A;
	int maxClaims = run->needPairs ? MAX_CLAIMS_PAIRED : MAX_CLAIMS;
	u64 endMs = osGetTime() + timeoutMs;
	for (int claim = 0;; claim++) {
		bool lastClaim = claim + 1 >= maxClaims;
		LightLock_Lock(&g_camera.lock);
		u32 first[CAM_PORT_COUNT];
		if (claim == 0 && run->ready.predicted) {
			first[CAM_PORT_A] = run->ready.firstFrame[CAM_PORT_A];
			first[CAM_PORT_B] = run->ready.firstFrame[CAM_PORT_B];
		} else {
			next_free_frames(first, run->stereo);
		}
		// Not before the exposure has settled on the view port's sensor. The partner frame moves with it.
		s32 lag = (s32)(g_camera.exposureWriteFrame + EXPOSURE_SETTLE_FRAMES - first[viewIndex]);
		if (lag > 0) {
			first[CAM_PORT_A] += (u32)lag;
			first[CAM_PORT_B] += (u32)lag;
		}
		start_claim(run, first, lastClaim);
		LightLock_Unlock(&g_camera.lock);
		run->claimMs = osGetTime();
		u32 leftMs = endMs > run->claimMs ? (u32)(endMs - run->claimMs) : 1;
		LightEvent_WaitTimeout(&g_camera.burst.done, (s64)leftMs * NANOSECONDS_PER_MILLISECOND);
		LightLock_Lock(&g_camera.lock);
		g_camera.burst.active = false;
		bool mismatch = g_camera.burst.mismatch;
		for (int port = 0; port < CAM_PORT_COUNT; port++) {
			run->received[port] = g_camera.burst.received[port];
			run->lost[port] = g_camera.burst.lost[port];
		}
		LightLock_Unlock(&g_camera.lock);
		run->doneMs = osGetTime();
		if (lastClaim || run->doneMs >= endMs) {
			break;
		}
		// A pair came off the claim: it is claimed again from the pairs found.
		if (mismatch) {
			run->reclaims++;
			continue;
		}
		bool allIn = run->received[CAM_PORT_A] >= frameCount && run->received[CAM_PORT_B] >= frameCount;
		if (run->needPairs && allIn) {
			int paired = count_pairs_at_one_moment(frameCount, run->received[CAM_PORT_A]);
			if (paired < frameCount) {
				run->reclaims++;
				continue;
			}
		}
		break;
	}
}

// 0 when every frame came, DUAL_BURST_PARTIAL when camera A got enough to merge, else a timeout.
static Result received_result(const BurstRun *run)
{
	bool allA = run->received[CAM_PORT_A] >= run->requestedCount;
	bool allB = !run->stereo || run->received[CAM_PORT_B] >= run->requestedCount;
	if (allA && allB) {
		return 0;
	}
	if (run->received[CAM_PORT_A] >= PARTIAL_MIN_FRAMES) {
		return DUAL_BURST_PARTIAL;
	}
	return DUAL_BURST_TIMEOUT;
}

// The exposure behind the frames: an HDR burst's own times, else the stream's.
static void record_exposure(const BurstRun *run)
{
	RawShotInfo *info = run->info;
	const HdrRun *hdr = &run->hdr;
	info->lines = hdr->requested ? hdr->lines[CAM_PORT_A] : g_camera.exposureLines;
	info->cameraLines[RAW_SHOT_CAMERA_A] = info->lines;
	info->cameraLines[RAW_SHOT_CAMERA_B] = hdr->requested ? hdr->lines[CAM_PORT_B] : g_camera.exposureLines;
	info->gain = hdr->requested ? hdr->liveGain : g_camera.exposureGain;
}

// Reads a sensor's timing registers. Returns the result of the fine time's read.
static Result read_timing(u8 device, u16 timing[RAW_SHOT_TIMING_COUNT])
{
	Result fineTimeRead = 0;
	for (int i = 0; i < RAW_SHOT_TIMING_COUNT; i++) {
		Result readResult = sensor_i2c_read16(device, SENSOR_TIMING_REGISTERS[i], &timing[i]);
		if (i == RAW_SHOT_FINE_TIME) {
			fineTimeRead = readResult;
		}
	}
	return fineTimeRead;
}

// Reads what each streaming sensor ran during the burst, before an HDR burst's times go back. Reports whether
// a sensor taking part had its own AE / AWB on, or a fine integration time that is not Pro Cam's.
static void read_sensor_state(const BurstRun *run, bool *autoExposureOn, bool *foreignFineTime)
{
	RawShotInfo *info = run->info;
	u8 devices[CAM_PORT_COUNT];
	int deviceCount = sensor_exposure_stream_devices(devices);
	*autoExposureOn = false;
	*foreignFineTime = false;
	for (int port = 0; port < deviceCount; port++) {
		CamPort *camPort = &g_camera.ports[port];
		bool takesPart = port == CAM_PORT_A || run->stereo;
		info->received[port] = run->received[port];
		info->bufferError[port] = camPort->bufferErrors != run->bufferErrorsAtStart[port] ||
		                          camPort->restarts != run->restartsAtStart[port] || run->lost[port];
		sensor_i2c_mcu_read(devices[port], SENSOR_MCU_SEQ_MODE, &info->sequencerMode[port]);
		if (takesPart && (info->sequencerMode[port] & SENSOR_SEQ_AE_AWB)) {
			*autoExposureOn = true;
		}
		sensor_i2c_read16(devices[port], SENSOR_REG_COARSE_TIME, &info->linesBack[port]);
		Result fineTimeRead = read_timing(devices[port], info->timing[port]);
		if (takesPart && R_SUCCEEDED(fineTimeRead) &&
		    info->timing[port][RAW_SHOT_FINE_TIME] != sensor_exposure_fine_time()) {
			*foreignFineTime = true;
		}
	}
}

// How far a gap lies from a whole number of frames, in frames.
static double off_whole_frames(double gapTicks, double periodTicks)
{
	double gapFrames = gapTicks / periodTicks;
	double fraction = gapFrames - (double)(long)(gapFrames + 0.5);
	if (fraction < 0) {
		fraction = -fraction;
	}
	return fraction;
}

// Failures a fresh start does not cure count towards giving the dual stream up. Sensors out of step restart
// by themselves. A whole burst resets the count.
static void count_burst_result(const BurstRun *run, Result result, bool outOfStep)
{
	if (result == 0) {
		g_camera.burstFails = 0;
		return;
	}
	bool unpairedInStep = result == DUAL_BURST_UNPAIRED && !outOfStep;
	if (run->received[CAM_PORT_B] == 0 || result == DUAL_BURST_TIMEOUT || result == DUAL_BURST_SENSOR_AE ||
	    unpairedInStep) {
		burst_failed();
	}
}

// Sorts the pairs by their frame ticks. A pair finished within the pair window is from one moment (pairMask).
// A gap of whole frames means frame numbers claimed apart. A gap of a fraction of a frame means the sensors
// are out of step: the next cam_stream_apply restarts the dual stream, which brings them back in step.
// Returns whether they are out of step.
static bool check_pairs(BurstRun *run, double periodTicks, Result *result)
{
	RawShotInfo *info = run->info;
	int pairCount = run->received[CAM_PORT_A];
	if (run->received[CAM_PORT_B] < pairCount) {
		pairCount = run->received[CAM_PORT_B];
	}
	if (pairCount > run->frameCount) {
		pairCount = run->frameCount;
	}
	int outOfStep = 0;
	double skewSumMs = 0;
	for (int i = 0; i < pairCount; i++) {
		u64 gap =
		        tick_gap(g_camera.burst.frameTicks[CAM_PORT_A][i], g_camera.burst.frameTicks[CAM_PORT_B][i]);
		double gapTicks = (double)gap;
		double gapMs = gapTicks / CPU_TICKS_PER_MSEC;
		if (is_pair_at_one_moment(gapTicks, periodTicks)) {
			info->pairMask |= 1u << i;
			info->paired++;
			skewSumMs += gapMs;
			if (gapMs > info->skewMaxMs) {
				info->skewMaxMs = (float)gapMs;
			}
		} else if (off_whole_frames(gapTicks, periodTicks) > 1.0 / CAM_PAIR_WINDOW_DIVISOR) {
			outOfStep++;
		}
	}
	if (info->paired) {
		info->skewMs = (float)(skewSumMs / info->paired);
	}
	if (outOfStep) {
		g_camera.dualDesync = true;
	}
	if (run->needPairs && R_SUCCEEDED(*result) && info->paired < pairCount) {
		*result = DUAL_BURST_UNPAIRED;
	}
	return outOfStep != 0;
}

// The dual set-up before the claim: the other camera's port started and an HDR burst's times written.
// Returns 0, or the result the burst ends with.
static Result prepare_dual(BurstRun *run)
{
	if (g_camera.dual) {
		u32 neededPorts = run->stereo ? CAM_PORT_MASK_BOTH : CAM_PORT_MASK_A;
		HdrRun *hdr = run->hdr.requested ? &run->hdr : NULL;
		// A failed set-up leaves the shot to the caller's fallback.
		if (R_FAILED(ready_begin(neededPorts, &run->ready, hdr))) {
			hdr_restore(&run->hdr);
			ready_end(&run->ready);
			if (run->stereo) {
				burst_failed();
			}
			return DUAL_BURST_NO_STREAM;
		}
	}
	// A time that did not reach a sensor: its frames would not be what they say.
	if (run->hdr.requested && R_FAILED(run->hdr.writeResult)) {
		hdr_restore(&run->hdr);
		ready_end(&run->ready);
		return run->hdr.writeResult;
	}
	return 0;
}

// What came of the claimed frames: whether they all came, the exposure behind them, and whether a sensor's
// own AE / AWB spoilt them. The live exposure goes back and the other camera's port stops on the way.
static Result judge_burst_result(BurstRun *run)
{
	RawShotInfo *info = run->info;
	Result result = received_result(run);
	record_exposure(run);
	info->reclaims = (u8)run->reclaims;
	bool autoExposureOn;
	bool foreignFineTime;
	read_sensor_state(run, &autoExposureOn, &foreignFineTime);
	hdr_restore(&run->hdr);
	ready_end(&run->ready);
	// Such a sensor's frames carry the module's exposure and colour gains, not the ones reported: a merge
	// would mix them, and the DNG would describe the wrong ones.
	if (autoExposureOn || foreignFineTime) {
		result = DUAL_BURST_SENSOR_AE;
		sensor_exposure_ask_guard();
	}
	return result;
}

// dual_burst_take and dual_burst_take_hdr. hdrLines: NULL, or camera A's and B's integration lines.
static Result burst_run(int frameCount, u8 *const *outA, u8 *const *outB, RawShotInfo *info, u32 timeoutMs,
                        const u16 *hdrLines)
{
	RawShotInfo unusedInfo;
	if (!info) {
		info = &unusedInfo;
	}
	memset(info, 0, sizeof *info);
	if (frameCount < 1 || !outA || !stream_has_full_raw_frames()) {
		return DUAL_BURST_NO_STREAM;
	}
	BurstRun run;
	memset(&run, 0, sizeof run);
	run.requestedCount = frameCount;
	// More than CAM_BURST_MAX_FRAMES gives a partial burst.
	run.frameCount = frameCount > CAM_BURST_MAX_FRAMES ? CAM_BURST_MAX_FRAMES : frameCount;
	run.outA = outA;
	run.outB = outB;
	run.info = info;
	run.stereo = g_camera.dual && outB;
	if (hdrLines) {
		// Both cameras at one moment need the dual stream: full frames, no sensor mode switch at the shutter.
		if (!run.stereo) {
			return DUAL_BURST_NO_STREAM;
		}
		hdr_prepare(&run.hdr, hdrLines, run.frameCount, &timeoutMs);
	}
	// A merge aligns its frames itself; 3D and HDR need every pair from one moment.
	run.needPairs = run.stereo && (hdrLines || run.frameCount == 1);
	run.startMs = osGetTime();
	wait_out_unverified_window(run.startMs);
	wait_exposure_settled();
	Result prepared = prepare_dual(&run);
	if (prepared != 0) {
		return prepared;
	}
	for (int port = 0; port < CAM_PORT_COUNT; port++) {
		run.bufferErrorsAtStart[port] = g_camera.ports[port].bufferErrors;
		run.restartsAtStart[port] = g_camera.ports[port].restarts;
	}
	claim_frames(&run, timeoutMs);
	double periodTicks = burst_period(run.received[CAM_PORT_A]);
	// Camera A takes part in every burst: its frame time.
	burst_release((u64)frame_ms(&g_camera.ports[CAM_PORT_A]));
	Result result = judge_burst_result(&run);
	if (run.stereo) {
		bool outOfStep = check_pairs(&run, periodTicks, &result);
		count_burst_result(&run, result, outOfStep);
	}
	return result;
}

Result dual_burst_take(int frameCount, u8 *const *outA, u8 *const *outB, RawShotInfo *info, u32 timeoutMs)
{
	return burst_run(frameCount, outA, outB, info, timeoutMs, NULL);
}

Result dual_burst_take_hdr(int frameCount, u8 *const *outA, u8 *const *outB, RawShotInfo *info, u32 timeoutMs,
                           u16 linesA, u16 linesB)
{
	const u16 hdrLines[CAM_PORT_COUNT] = {linesA, linesB};
	return burst_run(frameCount, outA, outB, info, timeoutMs, hdrLines);
}
