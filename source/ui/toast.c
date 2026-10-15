#include "toast.h"
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>

#define TOAST_DURATION_MS 2500

char g_toastText[TOAST_TEXT_SIZE];
u64 g_toastUntil;

void toast_show(const char *format, ...)
{
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(g_toastText, sizeof g_toastText, format, arguments);
	va_end(arguments);
	g_toastUntil = osGetTime() + TOAST_DURATION_MS;
}
