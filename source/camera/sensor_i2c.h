// Sensor access over i2c::CAM: registers, MCU variables and the raw output switch.
#pragma once
#include <3ds.h>
#include "cam_settings.h"

// Sensor registers (16-bit address and value)
#define SENSOR_REG_FRAME_LENGTH 0x300A // FRAME_LENGTH_LINES
#define SENSOR_REG_LINE_LENGTH 0x300C // LINE_LENGTH_PCK
#define SENSOR_REG_COARSE_TIME 0x3012 // COARSE_INTEGRATION_TIME, in lines
#define SENSOR_REG_FINE_TIME 0x3014 // FINE_INTEGRATION_TIME, in pixel clocks
#define SENSOR_REG_GAIN_GLOBAL 0x3028 // analog gains: global, then one per colour up to SENSOR_REG_GAIN_LAST
#define SENSOR_REG_GAIN_GREEN 0x302A
#define SENSOR_REG_GAIN_LAST 0x3030
#define SENSOR_REG_GAIN_STEP 2 // the gain registers are 16 bits wide
#define SENSOR_REG_READ_MODE 0x3040
#define SENSOR_READ_MODE_Y_ODD_INC_MASK 0x1C // y_odd_inc: 1 reads every row, 3 every other row pair
#define SENSOR_READ_MODE_Y_ODD_INC_SHIFT 2
#define SENSOR_READ_ROWS_ALL (1 << SENSOR_READ_MODE_Y_ODD_INC_SHIFT)
#define SENSOR_READ_ROWS_SKIPPED (3 << SENSOR_READ_MODE_Y_ODD_INC_SHIFT)

// Sensor MCU variables
#define SENSOR_MCU_SEQ_MODE 0xA102 // SEQ_MODE
#define SENSOR_SEQ_AE_AWB 0x05 // SEQ_MODE bits of the sensor's own auto exposure and white balance
#define SENSOR_SEQ_MODE_AUTO 0x0F // SEQ_MODE as the camera module runs it

// The camera module's session with the sensors' I2C bus.
#define I2C_SERVICE_NAME "i2c::CAM"

// I2C device of each sensor
#define SENSOR_DEVICE_INNER 1
#define SENSOR_DEVICE_OUT1 2
#define SENSOR_DEVICE_OUT2 4

// Read mode, frame length, line length, coarse and fine time.
#define SENSOR_TIMING_REGISTER_COUNT 5
extern const u16 SENSOR_TIMING_REGISTERS[SENSOR_TIMING_REGISTER_COUNT];

// The I2C device of each camera.
extern const u8 SENSOR_I2C_DEVICES[CAM_COUNT];

// Uses this i2c::CAM session, for example a copy of the camera module's own. Without one, the first access
// opens a session of its own.
void sensor_i2c_set_session(Handle session);
bool sensor_i2c_has_session(void);
void sensor_i2c_close_session(void);

Result sensor_i2c_read16(u8 device, u16 address, u16 *value);
Result sensor_i2c_write16(u8 device, u16 address, u16 value);

// MCU variables take two register accesses. Another thread's access between them reads the wrong variable.
Result sensor_i2c_mcu_write(u8 device, u16 variable, u16 value);
Result sensor_i2c_mcu_read(u8 device, u16 variable, u16 *value);

// One MCU variable of a camera. Only while the camera thread is idle and outside a start's window.
Result sensor_i2c_camera_mcu_read(int camera, u16 variable, u16 *value);

// Switches a camera between raw Bayer output and the camera module's own format.
Result sensor_i2c_set_raw_output(int camera, bool on);
