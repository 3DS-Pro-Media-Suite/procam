// The camera control thread: applies settings, switches streams, takes the shot and stops the stream while
// the app is away.
#pragma once
#include <3ds.h>
#include "cam_settings.h"
#include "settings.h"

void camera_control_init(void);
// Opens the camera, takes raw access to the sensors unless on Citra, and starts the stream for the live
// settings on the calling thread. displayReady: the live view has its buffers; without them the camera stays
// off.
void camera_control_start(bool onCitra, bool displayReady);
// The camera opened and runs. Else camera_control_error says why.
bool camera_control_ok(void);
Result camera_control_error(void);
// The camera is changing, resetting or shooting, or the app is away. The main loop leaves the sensors alone.
bool camera_control_changing(void);
// What the viewfinder reports as applying camera settings.
bool camera_control_applying(void);
void camera_control_request(const CamSettings *settings);
// A setting outside CamSettings changed (the mode, the 60 fps switch). modeChanged: the mode is chosen anew.
void camera_control_request_stream(bool modeChanged);
// Main loop, while the raw stream runs and the camera is not changing: hands stream repairs to the thread.
void camera_control_poll_stream(u64 nowMs);
// Main loop: no frame for too long means a wedged sensor, and the camera thread resets the driver.
void camera_control_watchdog(void);
void camera_control_exit(void);
// HOME and sleep: the stream stops while the app is away.
void camera_control_apt_hook(APT_HookType type, void *param);
// The system keyboard is open: the stream keeps running behind it.
void camera_control_set_in_applet(bool open);
// MERGE, HDR or 3D runs on one camera because its dual stream is down.
bool camera_control_dual_lost(const Settings *settings);
// Asks for the shot. It is taken on the next loop, once the UI has drawn a frame.
void camera_control_request_shot(void);
// Main loop, first thing: takes the shot asked for in the last loop.
void camera_control_fire_pending(void);
// While the main loop holds the lock no camera change starts, so a live frame can go to the preview worker.
void camera_control_lock(void);
void camera_control_unlock(void);
// The start time of the running capture, or 0.
u64 camera_control_shooting_since(void);
// What the running capture takes (MODE_*).
int camera_control_shooting_mode(void);
// The camera thread's last failure, cleared once taken. 0: none.
Result camera_control_take_failure(void);
