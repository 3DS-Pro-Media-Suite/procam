// Pushes settings into the camera service: trimming, metering windows and image controls. Internal to the
// camera library.
#pragma once
#include <3ds.h>
#include "cam_settings.h"

// Fills a CamSettings so that every field differs from any legal value (-128 in every field).
#define CAM_SETTINGS_UNSET_BYTE 0x80

// The functions taking `firstError` keep the first failure: they store theirs only while it holds success.

// Trim the raw readout of `port` to a width x height window in the middle of the active area.
// fastView: the sensor is in the fast live view and reads half the rows.
void cam_config_raw_trim(u32 port, int width, int height, bool fastView, Result *firstError);
// Set the live port's trimming for the stream that runs now.
void cam_config_stream_trim(Result *firstError);
// The live port's trimming and transfer size for the stream that runs now.
void cam_config_stream_transfer(Result *firstError);
// Push the image controls of `settings` that differ from `previous`.
Result cam_config_apply_image_controls(u32 cameraSelect, const CamSettings *settings,
                                       const CamSettings *previous);
