#include "capture_thread.h"
#include <3ds.h>
#include <stdatomic.h>
#include "cam_state.h"
#include "sensor_exposure.h"
#include "time_units.h"

// Restart storms are judged over the last CAM_RESTART_HISTORY_LENGTH restarts.
// A single stream with that many restarts within this time only recovers with a new start.
#define STORM_WINDOW_MS 2000
// A dual stream with that many restarts within this time is not stable on this console.
#define DUAL_UNSTABLE_WINDOW_MS 10000

// Buffer errors restart a port at most once in this time.
#define BUFFER_ERROR_RESTART_GAP_MS 100

// The frame period is a running average over about this many frames.
#define PERIOD_AVERAGE_FRAMES 8
// A gap longer than this many half periods spans a lost frame and is not a period.
#define PERIOD_MAX_GAP_HALVES 5

// The pair window while the period is not known yet.
#define PAIR_WINDOW_FALLBACK_MS 5

#define FPS_WINDOW_FRAMES 15

// A dual restart waits for the other port's next frame, unless that port is this late as well.
#define OTHER_PORT_LATE_PERIODS 3
#define OTHER_PORT_LATE_MARGIN_MS 50
#define FALLBACK_PERIOD_MS 40

#define PAUSED_WAIT_NS (4 * NANOSECONDS_PER_MILLISECOND)
#define HURRY_WAIT_NS (20 * NANOSECONDS_PER_MILLISECOND)
#define FRAME_WAIT_NS (100 * NANOSECONDS_PER_MILLISECOND)
#define PAUSE_POLL_NS (2500 * NANOSECONDS_PER_MICROSECOND)
// The pause waits at most a second for the thread to let go.
#define PAUSE_POLL_COUNT 400

// What a wait handle stands for: a port's receive, a port's buffer error event or the wake event.
enum {
	WAIT_RECEIVE = 0,
	WAIT_BUFFER_ERROR = WAIT_RECEIVE + CAM_PORT_COUNT,
	WAIT_WAKE = WAIT_BUFFER_ERROR + CAM_PORT_COUNT,
	WAIT_SOURCE_COUNT,
};

typedef enum {
	RESTART_NONE,
	RESTART_KICKED,
	RESTART_BUFFER_ERROR,
	RESTART_STALLED,
} RestartReason;

static bool port_in_mask(u32 portMask, int portIndex)
{
	return (portMask >> portIndex) & 1;
}

static bool both_ports(u32 portMask)
{
	return portMask == CAM_PORT_MASK_BOTH;
}

void capture_thread_reset_port(CamPort *port, u64 now)
{
	port->fill = 0;
	port->reading = -1;
	port->ready = -1;
	port->newFrame = false;
	port->armedDestination = NULL;
	port->armedSlot = -1;
	port->resetPeriodAverage = false;
	port->stopAfterBurst = false;
	port->restartDue = false;
	port->restartForBurst = false;
	port->framesDone = 0;
	port->periodTicks = 0;
	port->lastTick = 0;
	port->fpsStart = now;
	port->lastFrameMs = now;
	port->fpsFrames = 0;
}

// CAM_RESTART_HISTORY_LENGTH restarts within windowMs raise the flag, and the history starts again.
static void note_restart(CamRestartHistory *history, u64 now, u32 windowMs, _Atomic bool *flag)
{
	u32 count = atomic_fetch_add(&history->count, 1) + 1;
	history->times[(count - 1) % CAM_RESTART_HISTORY_LENGTH] = now;
	// The slot written next holds the oldest of the last restarts.
	u64 span = now - history->times[count % CAM_RESTART_HISTORY_LENGTH];
	bool historyFull = count >= CAM_RESTART_HISTORY_LENGTH;
	if (historyFull && span < windowMs && !*flag) {
		*flag = true;
		history->count = 0;
	}
}

// Only the handle: StopCapture is what cancels the receive itself.
static void close_receive(CamPort *port)
{
	if (port->receiveEvent) {
		svcCloseHandle(port->receiveEvent);
		port->receiveEvent = 0;
	}
}

// StopCapture also cancels the port's pending receive. The capture thread re-arms it right after.
static void restart_port(int portIndex, bool countsTowardStorm)
{
	CamPort *port = &g_camera.ports[portIndex];
	u32 cameraPort = g_camera.portIds[portIndex];
	CAMU_StopCapture(cameraPort);
	close_receive(port);
	LightLock_Lock(&g_camera.lock);
	if (port->armedSlot >= 0 && g_camera.burst.active) {
		g_camera.burst.lost[portIndex] = true;
	}
	port->armedDestination = NULL;
	port->armedSlot = -1;
	// Lost frames break this port's numbering against the other port, so the pair offset is found afresh.
	g_camera.pairAtMs = 0;
	LightLock_Unlock(&g_camera.lock);
	// Clears the port's FIFO and its error flags.
	CAMU_ClearBuffer(cameraPort);
	// An overrun while stopped is no news.
	if (port->errorEvent) {
		svcClearEvent(port->errorEvent);
	}
	CAMU_StartCapture(cameraPort);
	u64 now = osGetTime();
	port->lastRestartMs = now;
	port->lastFrameMs = now;
	port->restartDue = false;
	port->restarts++;
	g_camera.restarts++;
	// The exposure guard runs once the module had its chance to switch AE / AWB back on.
	g_camera.unverifiedUntil = now + CAM_UNVERIFIED_AFTER_START_MS;
	sensor_exposure_ask_guard();
	if (countsTowardStorm && !g_camera.dual) {
		note_restart(&g_camera.singleRestartHistory, now, STORM_WINDOW_MS, &g_camera.restartStorm);
	}
	if (countsTowardStorm && g_camera.dual && both_ports(g_camera.streamingPorts)) {
		note_restart(&g_camera.dualRestartHistory, now, DUAL_UNSTABLE_WINDOW_MS, &g_camera.dualBroken);
	}
}

// Where a port's next receive goes: the frame after framesDone. A frame of a running burst goes into the
// caller's buffer, never into the ring, because the live view patches ring frames in place. Other frames go
// into the ring, or into the sink for a port without one. Returns NULL when the port is not armed.
static u8 *next_destination_locked(int portIndex)
{
	CamPort *port = &g_camera.ports[portIndex];
	// Huge when the frame comes before the burst.
	u32 burstSlot = port->framesDone + 1 - g_camera.burst.first[portIndex];
	port->armedDestination = NULL;
	port->armedSlot = -1;
	if (g_camera.burst.active && g_camera.burst.destinations[portIndex] &&
	    burstSlot < (u32)g_camera.burst.frameCount) {
		port->armedDestination = g_camera.burst.destinations[portIndex][burstSlot];
		port->armedSlot = (int)burstSlot;
		return port->armedDestination;
	}
	if (port->ring[0]) {
		return (u8 *)port->ring[port->fill];
	}
	port->armedDestination = g_camera.sink;
	return g_camera.sink;
}

static void start_receive(int portIndex, u8 *destination)
{
	CamPort *port = &g_camera.ports[portIndex];
	// A burst port whose sink is gone is being stopped.
	if (!destination) {
		port->receiveEvent = 0;
		return;
	}
	u32 frameBytes = (u32)(g_camera.frameWidth * g_camera.frameHeight * CAM_PIXEL_BYTES);
	Result result = CAMU_SetReceiving(&port->receiveEvent, destination, g_camera.portIds[portIndex],
	                                  frameBytes, (s16)g_camera.transferBytes);
	if (R_FAILED(result)) {
		port->receiveEvent = 0;
		LightLock_Lock(&g_camera.lock);
		port->armedDestination = NULL;
		port->armedSlot = -1;
		LightLock_Unlock(&g_camera.lock);
	}
}

static void arm_next_receive(int portIndex)
{
	LightLock_Lock(&g_camera.lock);
	u8 *destination = next_destination_locked(portIndex);
	LightLock_Unlock(&g_camera.lock);
	start_receive(portIndex, destination);
}

// The finished frame becomes the newest one for cam_stream_poll. The next one goes into a buffer nobody
// holds.
static void publish_ring_frame_locked(CamPort *port)
{
	port->ready = port->fill;
	port->newFrame = true;
	for (int i = 0; i < CAM_RING_FRAMES; i++) {
		if (i != port->ready && i != port->reading) {
			port->fill = i;
			break;
		}
	}
}

// Only the frame the current claim gave a slot counts. A receive armed under an earlier claim lands in a slot
// that the claimed frame overwrites later.
static bool is_claimed_burst_frame_locked(int portIndex, int slot)
{
	if (!g_camera.burst.active || slot < 0 || slot >= g_camera.burst.frameCount) {
		return false;
	}
	return g_camera.ports[portIndex].framesDone == g_camera.burst.first[portIndex] + (u32)slot;
}

static bool burst_complete_locked(void)
{
	bool complete = true;
	for (int i = 0; i < CAM_PORT_COUNT; i++) {
		if (g_camera.burst.destinations[i] && g_camera.burst.received[i] < g_camera.burst.frameCount) {
			complete = false;
		}
	}
	return complete;
}

// A port in stopMask (dual-ready's other camera) stops now, not after dual_burst_take returns.
static void finish_burst_locked(void)
{
	g_camera.burst.active = false;
	for (int i = 0; i < CAM_PORT_COUNT; i++) {
		if (port_in_mask(g_camera.burst.stopMask, i)) {
			g_camera.ports[i].stopAfterBurst = true;
		}
	}
	LightEvent_Signal(&g_camera.burst.done);
}

static void record_burst_frame(int portIndex, int slot, u64 tick)
{
	LightLock_Lock(&g_camera.lock);
	if (is_claimed_burst_frame_locked(portIndex, slot)) {
		g_camera.burst.frameTicks[portIndex][slot] = tick;
		g_camera.burst.received[portIndex]++;
		if (burst_complete_locked()) {
			finish_burst_locked();
		}
	}
	LightLock_Unlock(&g_camera.lock);
}

static void update_frame_period(CamPort *port, u64 tick)
{
	// The gap up to the first frame after a frame length change spans both lengths.
	if (atomic_exchange(&port->resetPeriodAverage, false)) {
		port->periodTicks = 0;
		return;
	}
	// The gap after a restart is not a period.
	if (!port->lastTick) {
		return;
	}
	u64 gap = tick - port->lastTick;
	u64 maxGap = port->periodTicks * PERIOD_MAX_GAP_HALVES / 2;
	if (!port->periodTicks) {
		port->periodTicks = gap;
	} else if (gap < maxGap) {
		port->periodTicks = (port->periodTicks * (PERIOD_AVERAGE_FRAMES - 1) + gap) / PERIOD_AVERAGE_FRAMES;
	}
}

// The averaged period, or the frame length's period while there is none yet.
static u64 expected_period_ticks(const CamPort *port)
{
	if (port->periodTicks) {
		return port->periodTicks;
	}
	double ticksPerMicrosecond = CPU_TICKS_PER_MSEC / (double)MICROSECONDS_PER_MILLISECOND;
	return (u64)(cam_state_frame_lines() * sensor_exposure_line_microseconds() * ticksPerMicrosecond);
}

// A burst pairs frame first[0] + k of one port with first[1] + k of the other. A pair seen with another
// offset means the claimed pairs are not from one moment, and dual_burst_take claims again.
static bool pair_breaks_burst_claim_locked(void)
{
	bool bothCameras = g_camera.burst.destinations[CAM_PORT_A] && g_camera.burst.destinations[CAM_PORT_B];
	if (!g_camera.burst.active || !bothCameras) {
		return false;
	}
	s32 claimedOffset = (s32)(g_camera.burst.first[CAM_PORT_B] - g_camera.burst.first[CAM_PORT_A]);
	if (g_camera.pairOffset == claimedOffset) {
		return false;
	}
	// 3D and HDR need every pair. A merge aligns every frame anyway, so it claims again only while that is
	// cheap.
	if (g_camera.burst.pairs) {
		return true;
	}
	return g_camera.burst.received[CAM_PORT_A] <= 1 && g_camera.burst.received[CAM_PORT_B] <= 1;
}

// When the other port finished within the pair window, the two frames are a pair. Their frame numbers give
// the ports' offset.
static void measure_pair(int portIndex, u64 tick, u64 now)
{
	const CamPort *other = &g_camera.ports[cam_state_other_port(portIndex)];
	u64 period = expected_period_ticks(&g_camera.ports[portIndex]);
	u64 gap = tick > other->lastTick ? tick - other->lastTick : other->lastTick - tick;
	if (!other->lastTick || gap >= period / CAM_PAIR_WINDOW_DIVISOR) {
		return;
	}
	LightLock_Lock(&g_camera.lock);
	u32 framesDoneA = g_camera.ports[CAM_PORT_A].framesDone;
	g_camera.pairOffset = (s32)(g_camera.ports[CAM_PORT_B].framesDone - framesDoneA);
	g_camera.pairAtMs = now;
	if (pair_breaks_burst_claim_locked()) {
		g_camera.burst.mismatch = true;
		g_camera.burst.active = false;
		LightEvent_Signal(&g_camera.burst.done);
	}
	LightLock_Unlock(&g_camera.lock);
}

static void update_fps(CamPort *port, u64 now)
{
	port->fpsFrames++;
	if (port->fpsFrames < FPS_WINDOW_FRAMES) {
		return;
	}
	// One more millisecond keeps the divisor above zero.
	u64 windowMs = now - port->fpsStart + 1;
	port->fps = port->fpsFrames * (float)MILLISECONDS_PER_SECOND / (float)windowMs;
	port->fpsFrames = 0;
	port->fpsStart = now;
}

// The next receive has to be in place before the sensor's next frame starts, inside the blanking, so it is
// armed before anything else.
static void handle_frame_done(int portIndex, u64 tick)
{
	CamPort *port = &g_camera.ports[portIndex];
	svcCloseHandle(port->receiveEvent);
	port->receiveEvent = 0;
	LightLock_Lock(&g_camera.lock);
	bool ringFrame = port->armedDestination == NULL;
	int armedSlot = port->armedSlot;
	port->framesDone++;
	if (ringFrame) {
		publish_ring_frame_locked(port);
	}
	u8 *destination = next_destination_locked(portIndex);
	LightLock_Unlock(&g_camera.lock);
	start_receive(portIndex, destination);

	u64 now = osGetTime();
	if (!ringFrame) {
		record_burst_frame(portIndex, armedSlot, tick);
	}
	update_frame_period(port, tick);
	if (both_ports(g_camera.streamingPorts)) {
		measure_pair(portIndex, tick, now);
	}
	port->lastTick = tick;
	port->lastFrameMs = now;
	port->frames++;
	update_fps(port, now);
}

static bool other_port_frame_due(int portIndex, u64 now)
{
	const CamPort *other = &g_camera.ports[cam_state_other_port(portIndex)];
	u64 periodMs =
	        other->periodTicks ? (u64)((double)other->periodTicks / CPU_TICKS_PER_MSEC) : FALLBACK_PERIOD_MS;
	u64 dueUntilMs = other->lastFrameMs + OTHER_PORT_LATE_PERIODS * periodMs + OTHER_PORT_LATE_MARGIN_MS;
	return now < dueUntilMs;
}

// A restart is three cam:u calls, and no re-arm can happen during them. In dual it waits for the moment
// right after the other port's frame: that port is armed and needs the thread only a whole frame later.
static void service_port(int portIndex, bool safeMoment)
{
	CamPort *port = &g_camera.ports[portIndex];
	if (port->stopAfterBurst) {
		return;
	}
	u64 now = osGetTime();
	RestartReason reason = RESTART_NONE;
	if (port->restartForBurst) {
		reason = RESTART_KICKED;
	} else if (port->restartDue && now >= port->lastRestartMs + BUFFER_ERROR_RESTART_GAP_MS) {
		reason = RESTART_BUFFER_ERROR;
	} else if (now > port->lastFrameMs + g_camera.stallMs) {
		reason = RESTART_STALLED;
	}
	if (reason == RESTART_NONE) {
		return;
	}
	if (both_ports(g_camera.streamingPorts) && !safeMoment && other_port_frame_due(portIndex, now)) {
		return;
	}
	if (reason == RESTART_KICKED) {
		port->restartForBurst = false;
	}
	// A restart that dual_burst_take asked for is no sign of trouble.
	restart_port(portIndex, reason != RESTART_KICKED);
}

// Stops a port for good once its burst frames are in. Only the capture thread does it, right after a frame
// of a port that keeps streaming: that port's next receive is armed, so a slow call cannot make it late.
static void stop_port(int portIndex)
{
	CamPort *port = &g_camera.ports[portIndex];
	CAMU_StopCapture(g_camera.portIds[portIndex]);
	close_receive(port);
	LightLock_Lock(&g_camera.lock);
	port->armedDestination = NULL;
	port->armedSlot = -1;
	g_camera.streamingPorts &= ~(1u << portIndex);
	LightLock_Unlock(&g_camera.lock);
	if (port->errorEvent) {
		svcClearEvent(port->errorEvent);
	}
	port->restartForBurst = false;
	port->restartDue = false;
	port->stopAfterBurst = false;
}

static void stop_requested_ports(void)
{
	for (int i = 0; i < CAM_PORT_COUNT; i++) {
		CamPort *port = &g_camera.ports[i];
		if (!port->stopAfterBurst) {
			continue;
		}
		if (port_in_mask(g_camera.streamingPorts, i)) {
			stop_port(i);
		} else {
			port->stopAfterBurst = false;
		}
	}
}

// A port that keeps streaming has its next receive armed a whole frame ahead, so right after its frame ports
// can be stopped. After the frame of a port being stopped, the port that stays must have finished its own
// frame just before (the sensors are frame-locked), unless nothing else streams.
static bool is_stop_moment(int finishedPort, u64 tick)
{
	if (!g_camera.ports[finishedPort].stopAfterBurst) {
		return true;
	}
	for (int i = 0; i < CAM_PORT_COUNT; i++) {
		const CamPort *staying = &g_camera.ports[i];
		if (i == finishedPort || !port_in_mask(g_camera.streamingPorts, i) || staying->stopAfterBurst) {
			continue;
		}
		u64 pairWindow = staying->periodTicks ? staying->periodTicks / CAM_PAIR_WINDOW_DIVISOR
		                                      : (u64)(CPU_TICKS_PER_MSEC * PAIR_WINDOW_FALLBACK_MS);
		if (!staying->lastTick || tick < staying->lastTick) {
			return false;
		}
		return tick - staying->lastTick < pairWindow;
	}
	return true;
}

// The port overran because its receive came too late. It stops, so service_port restarts it.
static void handle_buffer_error(int portIndex)
{
	CamPort *port = &g_camera.ports[portIndex];
	// The event can stay signalled, and the wait would spin.
	svcClearEvent(port->errorEvent);
	port->bufferErrors++;
	port->restartDue = true;
}

static void handle_wait_source(int source, u32 streaming, u64 tick)
{
	if (source < WAIT_BUFFER_ERROR) {
		int portIndex = source - WAIT_RECEIVE;
		handle_frame_done(portIndex, tick);
		// Right after a frame is the safe moment to restart the other port or to stop a burst port.
		if (both_ports(streaming)) {
			service_port(cam_state_other_port(portIndex), true);
		}
		if (is_stop_moment(portIndex, tick)) {
			stop_requested_ports();
		}
		return;
	}
	if (source < WAIT_WAKE) {
		handle_buffer_error(source - WAIT_BUFFER_ERROR);
	}
	// The wake event needs no work: the wait list is built again.
}

// The thread lets go of the receives. The caller's StopCapture cancels them.
static void idle_while_paused(void)
{
	for (int i = 0; i < CAM_PORT_COUNT; i++) {
		CamPort *port = &g_camera.ports[i];
		close_receive(port);
		if (port->armedDestination) {
			LightLock_Lock(&g_camera.lock);
			if (g_camera.burst.active && port->armedSlot >= 0) {
				g_camera.burst.lost[i] = true;
			}
			port->armedDestination = NULL;
			port->armedSlot = -1;
			LightLock_Unlock(&g_camera.lock);
		}
	}
	g_camera.busy = false;
	if (g_camera.wakeEvent) {
		svcWaitSynchronization(g_camera.wakeEvent, PAUSED_WAIT_NS);
	} else {
		svcSleepThread(PAUSED_WAIT_NS);
	}
}

// Arms the streaming ports and lists every receive and buffer error event, then the wake event. Sets hurry
// when a port has no receive (SetReceiving failed) or a restart due, so the thread looks again soon.
static int collect_wait_handles(u32 streaming, Handle *handles, int *sources, bool *hurry)
{
	int count = 0;
	for (int i = 0; i < CAM_PORT_COUNT; i++) {
		if (!port_in_mask(streaming, i)) {
			continue;
		}
		CamPort *port = &g_camera.ports[i];
		if (!port->receiveEvent) {
			arm_next_receive(i);
		}
		if (port->receiveEvent) {
			handles[count] = port->receiveEvent;
			sources[count] = WAIT_RECEIVE + i;
			count++;
		} else {
			*hurry = true;
		}
		if (port->restartDue) {
			*hurry = true;
		}
	}
	for (int i = 0; i < CAM_PORT_COUNT; i++) {
		if (port_in_mask(streaming, i) && g_camera.ports[i].errorEvent) {
			handles[count] = g_camera.ports[i].errorEvent;
			sources[count] = WAIT_BUFFER_ERROR + i;
			count++;
		}
	}
	if (g_camera.wakeEvent) {
		handles[count] = g_camera.wakeEvent;
		sources[count] = WAIT_WAKE;
		count++;
	}
	return count;
}

static void service_streaming_ports(void)
{
	u32 streaming = g_camera.streamingPorts;
	for (int i = 0; i < CAM_PORT_COUNT; i++) {
		if (port_in_mask(streaming, i)) {
			service_port(i, false);
		}
	}
}

// The thread makes no I2C calls, and of cam:u only SetReceiving, Stop / StartCapture and ClearBuffer: the
// cam:u session serves one request at a time, and a slow one would delay a re-arm.
void capture_thread_run(void *arg)
{
	while (!g_camera.quit) {
		// Set before streamWanted is read: a pause that then finds busy false knows the thread saw it.
		g_camera.busy = true;
		if (!g_camera.streamWanted) {
			idle_while_paused();
			continue;
		}
		u32 streaming = g_camera.streamingPorts;
		Handle waitHandles[WAIT_SOURCE_COUNT];
		int waitSources[WAIT_SOURCE_COUNT];
		bool hurry = false;
		int waitCount = collect_wait_handles(streaming, waitHandles, waitSources, &hurry);
		s32 signalled = -1;
		Result waitResult = -1;
		if (waitCount) {
			s64 timeout = hurry ? HURRY_WAIT_NS : FRAME_WAIT_NS;
			waitResult = svcWaitSynchronizationN(&signalled, waitHandles, waitCount, false, timeout);
		} else {
			svcSleepThread(HURRY_WAIT_NS);
		}
		u64 tick = svcGetSystemTick();
		// A timeout returns a code that R_SUCCEEDED accepts, so only 0 means a handle is signalled.
		if (waitResult == 0 && signalled >= 0 && signalled < waitCount) {
			handle_wait_source(waitSources[signalled], streaming, tick);
		} else if (waitResult != 0) {
			// Nothing came in time: a port waiting to be stopped goes now.
			stop_requested_ports();
		}
		service_streaming_ports();
	}
	for (int i = 0; i < CAM_PORT_COUNT; i++) {
		close_receive(&g_camera.ports[i]);
	}
	g_camera.busy = false;
}

void capture_thread_pause(void)
{
	g_camera.streamWanted = false;
	// The wake event breaks the thread's wait at once.
	if (g_camera.wakeEvent) {
		svcSignalEvent(g_camera.wakeEvent);
	}
	for (int i = 0; i < PAUSE_POLL_COUNT && g_camera.busy; i++) {
		svcSleepThread(PAUSE_POLL_NS);
	}
}

void capture_thread_resume(void)
{
	u64 now = osGetTime();
	LightLock_Lock(&g_camera.lock);
	for (int i = 0; i < CAM_PORT_COUNT; i++) {
		capture_thread_reset_port(&g_camera.ports[i], now);
	}
	// Frame numbers start again.
	g_camera.pairOffset = 0;
	g_camera.pairAtMs = 0;
	g_camera.exposureWriteFrame = 0;
	g_camera.hideFramesUntil = 0;
	LightLock_Unlock(&g_camera.lock);
	g_camera.unverifiedUntil = now + CAM_UNVERIFIED_AFTER_START_MS;
	g_camera.streamWanted = true;
	// The thread arms the receives now instead of after its idle wait.
	if (g_camera.wakeEvent) {
		svcSignalEvent(g_camera.wakeEvent);
	}
}
