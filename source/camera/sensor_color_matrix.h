// The colour matrix each console stores in its sensors' MCU variables: white-balanced camera RGB to linear
// sRGB.
#pragma once
#include <3ds.h>

// Reads the matrix once, from the sensor of `camera` (a CamId). It waits while a start's window is open,
// because the camera module may make its own MCU reads then. Only while the raw stream runs.
void sensor_color_matrix_poll(int camera);
// The matrix, row-major, or NULL until it has been read. It is the same for every camera.
const float *sensor_color_matrix(void);
// Changes when the matrix does.
u32 sensor_color_matrix_revision(void);
