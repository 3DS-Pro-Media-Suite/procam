// Exposure on the sensors: time and gain written, read back and guarded against the module's own AE.
#pragma once
#include <3ds.h>
#include "cam_settings.h"

// An exposure is whole lines (the coarse time) plus pixel clocks (the fine time). The fine time is the
// module's own in auto and the remainder of the chosen time in manual and shutter priority. "Lines" elsewhere
// means the coarse part alone.
#define SENSOR_FINE_TIME_AUTO 633 // the module's FINE_INTEGRATION_TIME on all three sensors
#define SENSOR_FULL_FRAME_LINES 518 // FRAME_LENGTH_LINES as the module programs it: 30 fps
#define SENSOR_LINES_MAX 0xFFF0 // the longest coarse time written
#define SENSOR_EXPOSURE_MARGIN_LINES 4 // a frame is at least this many lines longer than its exposure
#define SENSOR_GAIN_CODE_MIN 8 // gain x1
#define SENSOR_GAIN_CODE_MAX 126
#define SENSOR_GAIN_CODE_STEP 2 // the lowest bit of a gain code does nothing, so only even codes are written
#define SENSOR_STREAM_DEVICES_MAX 2 // a dual stream runs on both outer sensors

// A sensor that stops keeps its coarse time, and its next start waits that long for a first frame. A long
// wait times out and the sensor stops answering, so before a stop a longer time is cut to this.
#define SENSOR_PARK_LINES 100

// How sensor_exposure_gain_code rounds a gain to a code.
typedef enum {
	GAIN_ROUND_DOWN = -1, // the largest code at most the gain
	GAIN_ROUND_NEAREST = 0,
	GAIN_ROUND_UP = 1, // the smallest code at least the gain
} GainRounding;

// One sensor line in microseconds, from the frame rate. It is the unit of the coarse time.
float sensor_exposure_line_microseconds(void);

// The fine time in use, in lines. Add it to the coarse lines for the real time or an exposure ratio.
float sensor_exposure_fine_lines(void);
// The real time of an exposure of that many lines, in seconds.
float sensor_exposure_seconds(u32 lines);
// The fine time last written, or the module's own before that.
u16 sensor_exposure_fine_time(void);

// A code within the sensor's range, even.
u16 sensor_exposure_even_gain_code(int code);
// The real gain of a code (8 is x1).
float sensor_exposure_gain_factor(u16 code);
// The ISO of a gain code (x1 is ISO 100).
int sensor_exposure_gain_iso(u16 code);
// The even code for a gain.
u16 sensor_exposure_gain_code(float gain, GainRounding rounding);

// The time manual or shutter priority wants, as coarse lines and fine time. False in auto.
bool sensor_exposure_manual_time(const CamSettings *settings, u16 *lines, u16 *fine);
// Manual or shutter priority: the lines sensor_exposure_apply chose. False in auto, whose loop moves them.
bool sensor_exposure_held_lines(u16 *lines);
// The exposure a capture starts from. Manual and shutter priority: the chosen time, whose fine part is then
// the one written. Auto: the time the view sensor is using. Manual: the ISO's gain, else the view sensor's.
void sensor_exposure_metered(const CamSettings *settings, u16 *lines, u16 *gain);

// The sensor the live view shows and meters.
u8 sensor_exposure_view_device(void);
// The sensors the stream runs on. Returns their count.
int sensor_exposure_stream_devices(u8 devices[SENSOR_STREAM_DEVICES_MAX]);

// The exposure the view sensor is using right now, read back from its registers.
typedef struct {
	u16 lines;
	u16 gain;
	float seconds; // fine time included
	int iso;
	bool valid;
	bool foreign; // a streaming sensor's own AE or AWB changed its exposure: the guard is asked for
} SensorExposure;

Result sensor_exposure_read(SensorExposure *exposure);

// Puts the exposure the settings ask for on the streaming sensors, with their own AE and AWB off.
Result sensor_exposure_apply(const CamSettings *settings);
// The coarse time on the streaming sensors. During an HDR burst it is only noted: the burst writes it back.
Result sensor_exposure_set_lines(u16 lines);
// The analog gain on all four colours. During an HDR burst it is only noted.
Result sensor_exposure_set_gain(u16 code);
// The same analog gain on all four colours of one sensor.
Result sensor_exposure_write_gains(u8 device, u16 gain);
// An exposure write in two halves, so that several sensors share one wait in between: first the sensor's AE
// and AWB off, then, once they have stopped, the time and the gains.
Result sensor_exposure_sequencer_off(u8 device);
Result sensor_exposure_write_registers(u8 device, u16 lines, u16 gain, bool writeCoarse);
// Both halves on one sensor, with the wait between them.
Result sensor_exposure_write_device(u8 device, u16 lines, u16 gain);
// Cuts a long time to SENSOR_PARK_LINES before the streaming sensors stop.
void sensor_exposure_park(void);
// The same for these sensors, which run `lines`.
void sensor_exposure_park_devices(const u8 *devices, int deviceCount, u16 lines);

// The guard switches a sensor's own AE and AWB off again where the camera module turned them back on, and
// writes Pro Cam's exposure where something else changed it.
void sensor_exposure_ask_guard(void);
bool sensor_exposure_guard_wanted(void);
// The guard on the camera thread. An app that never calls it gets it from sensor_exposure_read.
Result sensor_exposure_guard(void);
Result sensor_exposure_run_guard(void);
// A (re)start's window is open: the module may still switch a sensor's AE or AWB back on.
bool sensor_exposure_unverified(void);
