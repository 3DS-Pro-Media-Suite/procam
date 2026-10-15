#include "raw_access.h"
#include <3ds.h>
#include <string.h>
#include "cam_state.h"
#include "sensor_i2c.h"

#define LUMA_INFO_TYPE 0x10000 // the svcGetProcessInfo type Luma3DS adds: the process name
#define PROCESS_NAME_LENGTH 8 // svcGetProcessInfo LUMA_INFO_TYPE: the name, not terminated
#define CAMERA_PROCESS_NAME "camera"
#define MAX_PROCESSES 64
#define MAX_CAMERA_MODULE_HANDLES 512
#define SERVICE_NAME_SIZE 16

// The operations of Luma3DS's custom SVCs.
typedef enum {
	SERVICEOP_STEAL_CLIENT_SESSION = 0,
	SERVICEOP_GET_NAME = 1
} ServiceOp;

typedef enum {
	PROCESSOP_GET_ALL_HANDLES = 0
} ProcessOp;

// Luma3DS custom SVC 0xB0, svcControlService.
__attribute__((naked)) static Result luma_control_service(ServiceOp op, void *argument, Handle handle)
{
	__asm__ volatile("svc 0xB0\n\tbx lr");
}

// Luma3DS custom SVC 0xB1, svcCopyHandle.
__attribute__((naked)) static Result luma_copy_handle(Handle *out, Handle outProcess, Handle in,
                                                      Handle inProcess)
{
	__asm__ volatile("str r0, [sp, #-4]!\n\tsvc 0xB1\n\tldr r2, [sp], #4\n\tstr r1, [r2]\n\tbx lr");
}

// Luma3DS custom SVC 0xB3, svcControlProcess. Returns the number of handles written (raw handle values of
// the target process), or a negative Result.
__attribute__((naked)) static Result luma_control_process(Handle process, ProcessOp op, u32 argument2,
                                                          u32 argument3)
{
	__asm__ volatile("svc 0xB3\n\tbx lr");
}

// A process's name, "?" when unavailable. name holds PROCESS_NAME_LENGTH + 1 bytes.
static void get_process_name(Handle process, char *name)
{
	s64 packedName = 0;
	strcpy(name, "?");
	if (R_SUCCEEDED(svcGetProcessInfo(&packedName, process, LUMA_INFO_TYPE))) {
		memcpy(name, &packedName, PROCESS_NAME_LENGTH);
		name[PROCESS_NAME_LENGTH] = 0;
	}
}

// Opens the camera module's process into *cameraProcess.
static Result find_camera_process(Handle *cameraProcess)
{
	*cameraProcess = 0;
	u32 processIds[MAX_PROCESSES];
	s32 processCount = 0;
	Result result = svcGetProcessList(&processCount, processIds, MAX_PROCESSES);
	if (R_FAILED(result)) {
		return result;
	}
	for (s32 i = 0; i < processCount; i++) {
		Handle process;
		char name[PROCESS_NAME_LENGTH + 1];
		if (R_FAILED(svcOpenProcess(&process, processIds[i]))) {
			continue;
		}
		get_process_name(process, name);
		if (!strcmp(name, CAMERA_PROCESS_NAME) && !*cameraProcess) {
			*cameraProcess = process;
		} else {
			svcCloseHandle(process);
		}
	}
	return *cameraProcess ? 0 : CAM_ERROR_NO_CAMERA_PROCESS;
}

// Copies every handle of the camera module and keeps the one of the i2c::CAM session.
static Result copy_i2c_session(Handle cameraProcess, Handle *i2cSession)
{
	static u32 moduleHandles[MAX_CAMERA_MODULE_HANDLES];
	Result handleCount =
	        luma_control_process(cameraProcess, PROCESSOP_GET_ALL_HANDLES, (u32)moduleHandles, 0);
	if (handleCount < 0) {
		return handleCount;
	}
	if (handleCount > MAX_CAMERA_MODULE_HANDLES) {
		return CAM_ERROR_TOO_MANY_HANDLES;
	}
	for (s32 i = 0; i < handleCount; i++) {
		Handle copy = 0;
		Result copyResult = luma_copy_handle(&copy, CUR_PROCESS_HANDLE, moduleHandles[i], cameraProcess);
		if (R_FAILED(copyResult)) {
			continue;
		}
		char serviceName[SERVICE_NAME_SIZE] = "";
		Result nameResult = luma_control_service(SERVICEOP_GET_NAME, serviceName, copy);
		if (R_SUCCEEDED(nameResult) && !strcmp(serviceName, I2C_SERVICE_NAME) && !*i2cSession) {
			*i2cSession = copy; // the one handle kept
			continue;
		}
		svcCloseHandle(copy);
	}
	return *i2cSession ? 0 : CAM_ERROR_NO_I2C_SESSION;
}

Result raw_access_steal_i2c_session(Handle *i2cSession)
{
	*i2cSession = 0;
	Handle cameraProcess;
	Result result = find_camera_process(&cameraProcess);
	if (R_FAILED(result)) {
		return result;
	}
	result = copy_i2c_session(cameraProcess, i2cSession);
	svcCloseHandle(cameraProcess);
	return result;
}
