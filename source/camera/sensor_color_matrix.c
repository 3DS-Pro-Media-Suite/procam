#include "sensor_color_matrix.h"
#include <3ds.h>
#include <string.h>
#include "raw10.h"
#include "sensor_exposure.h"
#include "sensor_i2c.h"

// The sensor's colour matrix is the left matrix plus a position times the delta matrix, in MCU variables.
// At the daylight end, position 1, its rows sum to 1, so it suits white-balanced RGB. Each 3DS has its own.
#define SENSOR_MATRIX_LEFT 0x2306
#define SENSOR_MATRIX_DELTA 0x231C
#define SENSOR_VARIABLE_BYTES 2
#define SENSOR_MATRIX_ONE 256.0f // the entries are signed 8.8 fixed point
// A row sum further from 1 means a bad read.
#define MATRIX_ROW_SUM_MIN 0.8f
#define MATRIX_ROW_SUM_MAX 1.25f

static _Atomic u32 g_colorMatrixRevision;
static float g_colorMatrix[COLOR_MATRIX_ENTRIES];
static bool g_haveColorMatrix;
static bool g_colorMatrixTried;

static bool read_matrix_variable(int camera, u16 variable, u16 *value)
{
	return R_SUCCEEDED(sensor_i2c_camera_mcu_read(camera, variable, value));
}

static void read_color_matrix(int camera)
{
	g_colorMatrixTried = true;
	float matrix[COLOR_MATRIX_ENTRIES];
	for (int i = 0; i < COLOR_MATRIX_ENTRIES; i++) {
		u16 leftVariable = SENSOR_MATRIX_LEFT + i * SENSOR_VARIABLE_BYTES;
		u16 deltaVariable = SENSOR_MATRIX_DELTA + i * SENSOR_VARIABLE_BYTES;
		u16 left = 0;
		u16 leftAgain = 0;
		u16 delta = 0;
		u16 deltaAgain = 0;
		// Each entry is read twice and trusted only when both reads agree.
		bool readOk = read_matrix_variable(camera, leftVariable, &left) &&
		              read_matrix_variable(camera, leftVariable, &leftAgain) &&
		              read_matrix_variable(camera, deltaVariable, &delta) &&
		              read_matrix_variable(camera, deltaVariable, &deltaAgain);
		if (!readOk || left != leftAgain || delta != deltaAgain) {
			return;
		}
		matrix[i] = ((s16)left + (s16)delta) / SENSOR_MATRIX_ONE;
	}
	for (int row = 0; row < RGB_CHANNELS; row++) {
		float *entries = &matrix[row * RGB_CHANNELS];
		float sum = entries[RGB_RED] + entries[RGB_GREEN] + entries[RGB_BLUE];
		if (sum < MATRIX_ROW_SUM_MIN || sum > MATRIX_ROW_SUM_MAX) {
			return;
		}
		for (int column = 0; column < RGB_CHANNELS; column++) {
			entries[column] /= sum;
		}
	}
	memcpy(g_colorMatrix, matrix, sizeof matrix);
	g_haveColorMatrix = true;
	g_colorMatrixRevision++;
}

void sensor_color_matrix_poll(int camera)
{
	if (!g_colorMatrixTried && !sensor_exposure_unverified()) {
		read_color_matrix(camera);
	}
}

// No 3x3 matrix removes the inner camera's near-infrared cast, so it is left as it is.
const float *sensor_color_matrix(void)
{
	return g_haveColorMatrix ? g_colorMatrix : NULL;
}

u32 sensor_color_matrix_revision(void)
{
	return g_colorMatrixRevision;
}
