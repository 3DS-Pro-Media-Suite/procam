// The capture thread: re-arms each port's receive, rotates the frame buffers and restarts stalled ports.
#pragma once
#include <3ds.h>
#include "cam_state.h"

// Clears a port's stream state: no frames yet, nothing armed. Call it with g_camera.lock held.
void capture_thread_reset_port(CamPort *port, u64 now);
// The thread's entry point. It streams while resumed and returns once g_camera.quit is set.
void capture_thread_run(void *arg);
// Stops streaming and waits until the thread has let go of the receives.
void capture_thread_pause(void);
// Streams again from fresh port state.
void capture_thread_resume(void);
