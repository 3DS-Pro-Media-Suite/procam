#include "toast.h"
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define TOAST_DURATION_MS 2500

// The camera thread shows toasts while the main thread draws them, and a u64 can tear on the ARM11.
static LightLock g_toastLock;
static char g_toastText[TOAST_TEXT_SIZE];
static u64 g_toastUntil;

void toast_init(void)
{
	LightLock_Init(&g_toastLock);
}

void toast_show(const char *format, ...)
{
	char text[TOAST_TEXT_SIZE];
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(text, sizeof text, format, arguments);
	va_end(arguments);
	u64 until = osGetTime() + TOAST_DURATION_MS;
	LightLock_Lock(&g_toastLock);
	memcpy(g_toastText, text, sizeof g_toastText);
	g_toastUntil = until;
	LightLock_Unlock(&g_toastLock);
}

bool toast_current(u64 nowMs, char text[TOAST_TEXT_SIZE])
{
	LightLock_Lock(&g_toastLock);
	bool showing = nowMs < g_toastUntil;
	if (showing) {
		memcpy(text, g_toastText, TOAST_TEXT_SIZE);
	}
	LightLock_Unlock(&g_toastLock);
	return showing;
}
