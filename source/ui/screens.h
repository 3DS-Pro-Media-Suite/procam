// The frame on both screens: the viewfinder on the top screen and the panels on the bottom one, or the other
// way round while the screens are swapped.
#pragma once
#include <citro2d.h>

typedef struct {
	C3D_RenderTarget *top;
	C3D_RenderTarget *topRight; // the right eye of the 3D top screen
	C3D_RenderTarget *bottom;
} Screens;

void screens_draw_frame(const Screens *screens);
