// A short message over the screen for a few seconds. The viewfinder and the gallery panel draw it.
#pragma once
#include <3ds.h>

#define TOAST_TEXT_SIZE 96

extern char g_toastText[TOAST_TEXT_SIZE];
extern u64 g_toastUntil;

// Shows a printf-style message.
void toast_show(const char *format, ...);
