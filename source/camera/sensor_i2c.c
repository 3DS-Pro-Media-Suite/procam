#include "sensor_i2c.h"
#include <3ds.h>
#include <string.h>
#include "cam_settings.h"
#include "cam_state.h"
#include "math_util.h"
#include "time_units.h"

// IPC command headers
#define SRV_GET_SERVICE_HANDLE_HEADER 0x00050100 // srv command 5 with four normal words
#define I2C_WRITE_REGISTER16_HEADER 0x000700C0 // i2c command 7 with three normal words
#define I2C_READ_REGISTER16_HEADER 0x000A0080 // i2c command 10 with two normal words

#define SERVICE_NAME_BYTES 8 // srv takes the name in two words
#define SRV_FLAG_DO_NOT_WAIT 1

// MCU variables are reached through an address register and a data register.
#define MCU_ADDRESS_REGISTER 0x098C
#define MCU_DATA_REGISTER 0x0990

#define MCU_OUTPUT_FORMAT_A 0x2755 // MODE_OUTPUT_FORMAT_A
#define MCU_OUTPUT_FORMAT_B 0x2757 // MODE_OUTPUT_FORMAT_B
#define OUTPUT_FORMAT_BAYER 0x0004
#define OUTPUT_FORMAT_MODULE 0x0002 // the camera module's own YUYV

// Bit 6 of this register sends the samples from before the image processor ("8+2 bypass").
#define REG_OUTPUT_BYPASS 0x3330
#define OUTPUT_BYPASS_8_PLUS_2 0x0040
#define OUTPUT_BYPASS_OFF 0x0000

// The sensor's MCU clears SEQ_CMD once it has run the command.
#define MCU_SEQ_CMD 0xA103
#define SEQ_CMD_REFRESH 5
#define SEQ_CMD_REFRESH_MODE 6
#define SEQ_CMD_POLL_NS (10 * NANOSECONDS_PER_MILLISECOND)
#define SEQ_CMD_POLL_COUNT 100 // one second

const u16 SENSOR_TIMING_REGISTERS[SENSOR_TIMING_REGISTER_COUNT] = {
        SENSOR_REG_READ_MODE,   SENSOR_REG_FRAME_LENGTH, SENSOR_REG_LINE_LENGTH,
        SENSOR_REG_COARSE_TIME, SENSOR_REG_FINE_TIME,
};

// Outer right is OUT1.
const u8 SENSOR_I2C_DEVICES[CAM_COUNT] = {
        SENSOR_DEVICE_OUT1,
        SENSOR_DEVICE_INNER,
        SENSOR_DEVICE_OUT2,
};

static Handle g_i2cSession;

// A new session replaces the current one.
void sensor_i2c_set_session(Handle session)
{
	if (g_i2cSession && g_i2cSession != session) {
		svcCloseHandle(g_i2cSession);
	}
	g_i2cSession = session;
}

bool sensor_i2c_has_session(void)
{
	return g_i2cSession != 0;
}

void sensor_i2c_close_session(void)
{
	if (g_i2cSession) {
		svcCloseHandle(g_i2cSession);
		g_i2cSession = 0;
	}
}

// srvGetServiceHandle waits forever when the service has no free session, and the camera module may hold
// i2c::CAM's only one. This is the same srv request with the flag that fails at once instead.
static Result open_session(void)
{
	if (g_i2cSession) {
		return 0;
	}
	const char *name = I2C_SERVICE_NAME;
	u32 *command = getThreadCommandBuffer();
	command[0] = SRV_GET_SERVICE_HANDLE_HEADER;
	memset(&command[1], 0, SERVICE_NAME_BYTES);
	memcpy(&command[1], name, strlen(name));
	command[3] = (u32)strlen(name);
	command[4] = SRV_FLAG_DO_NOT_WAIT;
	Result result = svcSendSyncRequest(*srvGetSessionHandle());
	if (R_FAILED(result)) {
		return result;
	}
	result = (Result)command[1];
	if (R_SUCCEEDED(result)) {
		g_i2cSession = command[3];
	}
	return result;
}

static Result send_register_command(u32 header, u8 device, u16 address, u16 value, u16 *readValue)
{
	Result result = open_session();
	if (R_FAILED(result)) {
		return result;
	}
	u32 *command = getThreadCommandBuffer();
	command[0] = header;
	command[1] = device;
	command[2] = address;
	command[3] = value;
	result = svcSendSyncRequest(g_i2cSession);
	if (R_FAILED(result)) {
		return result;
	}
	if (readValue) {
		*readValue = (u16)command[2];
	}
	return (Result)command[1];
}

Result sensor_i2c_read16(u8 device, u16 address, u16 *value)
{
	return send_register_command(I2C_READ_REGISTER16_HEADER, device, address, 0, value);
}

Result sensor_i2c_write16(u8 device, u16 address, u16 value)
{
	return send_register_command(I2C_WRITE_REGISTER16_HEADER, device, address, value, NULL);
}

Result sensor_i2c_mcu_write(u8 device, u16 variable, u16 value)
{
	Result result = sensor_i2c_write16(device, MCU_ADDRESS_REGISTER, variable);
	if (R_FAILED(result)) {
		return result;
	}
	return sensor_i2c_write16(device, MCU_DATA_REGISTER, value);
}

Result sensor_i2c_mcu_read(u8 device, u16 variable, u16 *value)
{
	Result result = sensor_i2c_write16(device, MCU_ADDRESS_REGISTER, variable);
	if (R_FAILED(result)) {
		return result;
	}
	return sensor_i2c_read16(device, MCU_DATA_REGISTER, value);
}

static u8 device_of_camera(int camera)
{
	return SENSOR_I2C_DEVICES[math_clamp_int(camera, 0, CAM_COUNT - 1)];
}

Result sensor_i2c_camera_mcu_read(int camera, u16 variable, u16 *value)
{
	return sensor_i2c_mcu_read(device_of_camera(camera), variable, value);
}

static Result run_sequencer_command(u8 device, u16 command)
{
	Result result = sensor_i2c_mcu_write(device, MCU_SEQ_CMD, command);
	if (R_FAILED(result)) {
		return result;
	}
	u16 pending = command;
	for (int i = 0; i < SEQ_CMD_POLL_COUNT && pending; i++) {
		svcSleepThread(SEQ_CMD_POLL_NS);
		sensor_i2c_mcu_read(device, MCU_SEQ_CMD, &pending);
	}
	// A sensor in standby never takes the command.
	if (pending) {
		return CAM_ERROR_SEQUENCER_TIMEOUT;
	}
	return 0;
}

static Result refresh_sequencer(u8 device)
{
	Result result = run_sequencer_command(device, SEQ_CMD_REFRESH_MODE);
	if (R_FAILED(result)) {
		return result;
	}
	return run_sequencer_command(device, SEQ_CMD_REFRESH);
}

// Raw output is the sensor's 10-bit Bayer data from before its image processor: 2 bytes per pixel (the 8
// high bits, then the 2 low bits), 648 pixels per line, RGGB. It needs the i2c::CAM session.
Result sensor_i2c_set_raw_output(int camera, bool on)
{
	u8 device = device_of_camera(camera);
	u16 format = on ? OUTPUT_FORMAT_BAYER : OUTPUT_FORMAT_MODULE;
	Result result = sensor_i2c_mcu_write(device, MCU_OUTPUT_FORMAT_A, format);
	if (R_SUCCEEDED(result)) {
		result = sensor_i2c_mcu_write(device, MCU_OUTPUT_FORMAT_B, format);
	}
	if (R_SUCCEEDED(result)) {
		result = refresh_sequencer(device);
	}
	u16 bypass = on ? OUTPUT_BYPASS_8_PLUS_2 : OUTPUT_BYPASS_OFF;
	Result bypassResult = sensor_i2c_write16(device, REG_OUTPUT_BYPASS, bypass);
	if (R_FAILED(result)) {
		return result;
	}
	return bypassResult;
}
