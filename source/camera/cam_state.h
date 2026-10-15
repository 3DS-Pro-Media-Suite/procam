// The camera library's shared state (ports, stream, dual and burst bookkeeping), its error codes and small
// shared helpers. Internal to the camera library.
#pragma once
#include <3ds.h>
#include "cam_settings.h"

#define CAM_RING_FRAMES 3
// The line time is held this long after a frame length change, while the frame rate it comes from settles.
#define CAM_LINE_HOLD_MS 1500
#define CAM_BURST_MAX_FRAMES 32 // frames per sensor one dual_burst_take takes at most
// Restart times kept to judge whether a stream is stable.
#define CAM_RESTART_HISTORY_LENGTH 4
// The camera module may switch the sensors' AE / AWB back on this long after a StartCapture.
#define CAM_UNVERIFIED_AFTER_START_MS 500
// Frames of the two ports that finish less than a frame period / CAM_PAIR_WINDOW_DIVISOR apart belong to one
// moment.
#define CAM_PAIR_WINDOW_DIVISOR 4

// ports[CAM_PORT_A] is the single stream, or camera A (OUT1, the right eye) in dual. ports[CAM_PORT_B] is
// camera B (OUT2, the left eye), dual only. Port masks have bit c set for ports[c].
#define CAM_PORT_COUNT 2
#define CAM_PORT_A 0
#define CAM_PORT_B 1
#define CAM_PORT_MASK_A (1u << CAM_PORT_A)
#define CAM_PORT_MASK_BOTH ((1u << CAM_PORT_COUNT) - 1)

// The camera library's own failures. Like every failed Result they are negative.
// The start and capture failures keep the small codes the toasts and the info page show.
enum {
	CAM_ERROR_NOT_INITIALIZED = (Result)-1,
	CAM_ERROR_START_CAPTURE = (Result)-2,
	CAM_ERROR_TIMED_OUT = (Result)-3, // the same code as DUAL_BURST_TIMEOUT
	CAM_ERROR_SEQUENCER_TIMEOUT = (Result)-4, // a sensor never took a sequencer command
	CAM_ERROR_NO_MEMORY = (Result)-5,
	CAM_ERROR_REFUSED = (Result)-6, // the camera is not set up for this capture
};

// The other failures, in the range of the DUAL_BURST_* results.
enum {
	CAM_ERROR_NOT_RAW = (Result)0xC8A0FFE3, // the call needs the raw stream
	CAM_ERROR_NO_SINK = (Result)0xC8A0FFE7, // a dual burst has no frame for the other camera's port
	CAM_ERROR_NO_CAMERA_PROCESS = (Result)0xC8A0FFE8, // raw access found no camera module
	CAM_ERROR_TOO_MANY_HANDLES =
	        (Result)0xC8A0FFE9, // the camera module has more handles than raw access lists
	CAM_ERROR_NO_I2C_SESSION = (Result)0xC8A0FFEA, // the camera module holds no i2c::CAM session
};

// One camera port's stream: a ring of CAM_RING_FRAMES frames for the live view, the receive waiting on the
// port, and its health.
typedef struct {
	// NULL on a port that only streams for bursts (the other camera of a dual stream).
	u16 *ring[CAM_RING_FRAMES];
	// fill, ready, reading, newFrame, armedDestination and armedSlot are guarded by g_camera.lock.
	int fill;
	int ready;
	int reading;
	bool newFrame;
	Handle receiveEvent; // the pending receive
	Handle errorEvent; // the port's buffer error event
	// Where the pending receive writes: a burst buffer or g_camera.sink. NULL: the ring (ring[fill]).
	u8 *armedDestination;
	int armedSlot; // that burst buffer's index (-1: the ring or the sink)
	// Restart the port for a burst: StopCapture cancels a receive still aimed at a burst buffer.
	_Atomic bool restartForBurst;
	// Stop the port once its burst frames are in. The capture thread does it.
	_Atomic bool stopAfterBurst;
	// A buffer error came too soon after the last restart: restart once that is long enough ago.
	bool restartDue;
	_Atomic u32 framesDone; // frames the port finished since the stream (re)started
	u64 lastTick; // system tick of the port's last frame
	u64 lastFrameMs; // osGetTime of the last frame or restart, for stall detection
	u64 lastRestartMs;
	u64 startMs; // osGetTime of the port's last StartCapture, by the stream or a restart
	u64 periodTicks; // the frame period, averaged
	// The frame length changed (an HDR burst): the period average starts again from the next frame.
	_Atomic bool resetPeriodAverage;
	_Atomic u32 frames;
	_Atomic u32 restarts;
	_Atomic u32 bufferErrors;
	u32 fpsFrames;
	u64 fpsStart;
	_Atomic float fps;
} CamPort;

// A capture thread re-arms the receive the moment a frame completes. Re-arming from the main loop, once per
// frame, comes too late: the sensor overruns its buffer and the stream keeps dying.
typedef struct {
	bool initialized;
	bool running; // configured and started (main thread only)
	_Atomic bool streamWanted; // the capture thread should stream
	_Atomic bool busy; // the capture thread is inside its streaming loop
	_Atomic bool quit;
	Thread thread;
	Handle wakeEvent; // wakes the capture thread: a pause, a resume, a port added for a burst
	// Guards the rings (fill, ready, reading, newFrame), armedDestination, the burst claim and the ports.
	LightLock lock;
	CamSettings settings;
	CamPort ports[CAM_PORT_COUNT];
	// Bit c set: ports[c] streams. Single: bit 0. Dual: the view's port, and both during a burst.
	_Atomic u32 streamingPorts;
	u32 portIds[CAM_PORT_COUNT]; // each port's camera service port
	// The frame of every streaming port (dual: always full frames).
	int frameWidth;
	int frameHeight;
	u32 transferBytes;
	u32 cameraSelect;
	u32 livePort; // the steady stream's port: the single stream's, or the dual live one
	_Atomic u32 restarts; // all ports
	Result lastError;
	const char *lastErrorCall;
	bool haveCalibration;
	CAMU_StereoCameraCalibrationData calibration;
	bool rawMode; // stream the sensor's raw 10-bit Bayer data (needs the i2c::CAM session)
	bool rawActive; // the running stream really is raw: the switch succeeded at the last start
	bool fastViewWanted; // fast live view wanted (raw, one camera only)
	bool fastViewOn; // the sensor is in the fast, row-skipping mode now (never in dual)
	// cam_stream_poll shows the view port's frames only after this framesDone, to hide an HDR burst's frames.
	_Atomic u32 hideFramesUntil;
	// The sensor's read mode and frame length outside the fast mode.
	u16 readModeNormal;
	u16 frameLinesNormal;
	_Atomic u32 stallMs; // a port without a frame for this long is restarted (longer for long exposures)

	// Dual: both outer sensors active, raw, with one frame timing and synchronised vsync. Only the view
	// camera's port streams; the other one joins for bursts ("dual-ready").
	bool dualWanted;
	_Atomic bool dual; // the stream is (being set up as) a dual one
	// Dual failed to start, kept restarting or its bursts kept failing: one camera until dual is turned off.
	_Atomic bool dualBroken;
	// A burst found the two sensors' frames apart: the next cam_stream_apply starts again to align them.
	_Atomic bool dualDesync;
	int view; // dual: the port the live view and the metering use (CAM_PORT_A or CAM_PORT_B)
	u16 dualFrameLines; // dual: the full frame's length on both sensors
	// Dual: each sensor's own read mode at the start (0: not read), its mirror bits kept.
	u16 readModeDual[CAM_PORT_COUNT];
	u16 frameLinesSaved[CAM_PORT_COUNT]; // OUT1's and OUT2's own frame length before dual
	u8 *sink; // dual: where the other port's frames go outside a burst (one frame)
	u32 burstFails; // dual bursts failed in a row
	// Dual: the times of the last restarts, to spot an unstable stream. Kept across the stream's restarts and
	// cleared when dual is turned off.
	u64 dualRestartTimes[CAM_RESTART_HISTORY_LENGTH];
	u32 dualRestartCount;
	// Camera B's framesDone minus camera A's, of two frames that finished together.
	_Atomic s32 pairOffset;
	_Atomic u64 pairAtMs; // osGetTime of the last such pair

	// The exposure as the app last wrote it, and the guard against the module switching the sensors' own
	// AE / AWB back on.
	u16 exposureLines;
	u16 exposureGain;
	u16 exposureFineTime; // the fine integration time written with it (0: none written yet)
	u16 heldLines; // the whole lines sensor_exposure_apply chose
	u16 aeR9Lines; // AE_R9 (MCU register 0x2222) as last written on the streaming sensors (0: not known)
	// The time sensor_exposure_park cut for a stop: auto exposure starts again from it.
	u16 parkedLines;
	// A single stream's last restart times, and whether a storm of them happened (cam_stream_restart_storm).
	u64 singleRestartTimes[CAM_RESTART_HISTORY_LENGTH];
	u32 singleRestartCount;
	_Atomic bool restartStorm;
	// The view port's framesDone at the last exposure write: bursts let it settle.
	_Atomic u32 exposureWriteFrame;
	_Atomic u64 unverifiedUntil; // a (re)start's window in which the module may switch AE / AWB back on
	// sensor_exposure_guard is wanted: a sensor's AE / AWB showed up on, or a port restarted.
	_Atomic bool guardRequested;
	// The guard is due from this osGetTime on (0: at once).
	_Atomic u64 guardDueMs;
	bool guardSeenPending; // the last sensor_exposure_read left a request pending
	// The app calls sensor_exposure_guard itself, so sensor_exposure_read never runs it as a fallback.
	bool guardByCaller;

	// An HDR burst holds the two dual sensors at their own integration times, and at a longer frame when a
	// time needs one, until it restores them. Live exposure changes are only noted meanwhile.
	_Atomic bool hdrOn;
	u16 hdrFrameLines; // the frame length both run meanwhile (0: no HDR burst, the dual frame)
	// The line time stays as it is until then: the frame rate it comes from still covers an HDR burst's
	// longer frames.
	_Atomic u64 lineHoldUntil;

	struct {
		_Atomic bool active;
		int frameCount;
		u8 *const *destinations[CAM_PORT_COUNT]; // NULL: that port takes no part
		u32 first
		        [CAM_PORT_COUNT]; // the framesDone of the frame each port receives into destinations[port][0]
		// Ports stopped once all their frames are in: the other camera when dual-ready, on the last try.
		u32 stopMask;
		// Every pair must be from one moment (3D, HDR): a pair off the claim claims again.
		bool pairs;
		// The capture thread found a pair whose frame numbers are not the claim's: the burst claims again.
		_Atomic bool mismatch;
		_Atomic int received[CAM_PORT_COUNT];
		u64 frameTicks[CAM_PORT_COUNT][CAM_BURST_MAX_FRAMES];
		_Atomic bool lost[CAM_PORT_COUNT]; // a restart cancelled a receive aimed at the burst
		LightEvent done;
	} burst;
} CamState;

// Run a camera service call. On failure remember the first failure and its call text in the caller's `err`
// and g_camera.lastErrorCall, but carry on: one unsupported control must not stop the rest.
// CAM_TRY_AS gives the call text itself. The info page and the start toast show it, so it stays the same
// when the code around the call is renamed.
#define CAM_TRY_AS(callText, call)                        \
	do {                                                  \
		Result camTryResult = (call);                     \
		if (R_FAILED(camTryResult) && R_SUCCEEDED(err)) { \
			err = camTryResult;                           \
			g_camera.lastErrorCall = (callText);          \
		}                                                 \
	} while (0)
#define CAM_TRY(call) CAM_TRY_AS(#call, call)

// The camera service's camera select and port of each CamId.
extern const u32 CAM_SERVICE_SELECT[CAM_COUNT];
extern const u32 CAM_SERVICE_PORT[CAM_COUNT];
extern CamState g_camera;

// The port the live view shows: in dual, the view sensor's port.
CamPort *cam_state_view_port(void);

// The dual stream's other port.
static inline int cam_state_other_port(int portIndex)
{
	return portIndex ^ 1;
}

// The frame length both dual sensors run now: an HDR burst's, or the full frame's.
u16 cam_state_frame_lines(void);
// Sleeps for a number of system ticks.
void cam_state_sleep_ticks(u64 ticks);
// Wait until the middle of a frame, so that frame timing writes reach both dual sensors on the same frame.
// lastFrameTick and periodTicks describe the live port; without them there is no wait.
void cam_state_wait_mid_frame(u64 lastFrameTick, u64 periodTicks);
// The capture thread's stall limit for frames of this length: a margin plus two frames.
u32 cam_state_stall_limit_ms(float frameSeconds);
