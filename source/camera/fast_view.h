// The 60 fps row-skipping live view of a single raw stream.
#pragma once
#include <3ds.h>

#define FAST_VIEW_FRAME_CUT 244 // lines the fast live view's frame is shorter than the full one

// Raw only: the 60 fps live view (640 x 240, row pairs skipped) from the next start of a single stream. The
// dual stream ignores it: it runs full frames all the time.
void fast_view_set_wanted(bool on);
bool fast_view_active(void); // the running stream is in the fast mode (frames are half height)
// Switches the running single raw stream to or from the fast mode (camera thread). Does nothing on a dual
// stream.
Result fast_view_switch(bool on);
// Takes the sensor's full frame length, and writes the default back when the register holds a stretched one.
void fast_view_fix_frame_length(void);
// Switches the view sensor's read mode and frame length. The port must be stopped.
Result fast_view_set_sensor(bool on);
// Sets the stream's frame size for the mode the sensor runs: half height in the fast mode.
void fast_view_update_frame_size(void);
