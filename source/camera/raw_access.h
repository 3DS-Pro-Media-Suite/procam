// Reads the sensors past the camera service: Luma3DS custom SVCs find the camera sysmodule and copy its own
// "i2c::CAM" session into this process. Everything here only reads: listing processes and handles, and
// copying a handle (the camera module keeps its own).
#pragma once
#include <3ds.h>

// Finds the camera module and returns a copy of its i2c::CAM session in *i2cSession (0 when not found).
Result raw_access_steal_i2c_session(Handle *i2cSession);
