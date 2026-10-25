// A short message over the screen for a few seconds. The viewfinder and the gallery panel draw it.
#pragma once
#include <3ds.h>

#define TOAST_TEXT_SIZE 96

// Before any thread shows a toast.
void toast_init(void);
// Shows a printf-style message.
void toast_show(const char *format, ...);
// Copies the toast showing at nowMs (osGetTime) into text. Returns false when none is showing.
bool toast_current(u64 nowMs, char text[TOAST_TEXT_SIZE]);
