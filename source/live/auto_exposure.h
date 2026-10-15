// Reads the live exposure back and runs Pro Cam's own raw exposure loop on the live view's meter.
#pragma once
#include <3ds.h>
#include "cam_settings.h"
#include "sensor_exposure.h"

// The longest exposure the stream survives.
#define AUTO_EXPOSURE_MAX_SECONDS 1.0f

extern SensorExposure g_exposureNow;

// AUTO_EXPOSURE_MAX_SECONDS in sensor lines at the current line time.
float auto_exposure_max_lines(void);
// Main loop, while the raw stream runs and the camera is not changing.
void auto_exposure_poll(const CamSettings *settings, u64 nowMs);
// No raw stream: the exposure is unknown.
void auto_exposure_clear(void);
