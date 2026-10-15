// Anti-aliased shapes: polygons, circles, rounded rectangles, frames, triangles and arcs.
#pragma once
#include <3ds.h>

#define UI_FULL_TURN 6.2831853f // a full circle in radians

// Directions of ui_shapes_pointer.
enum {
	POINTER_DIRECTION_LEFT = -1,
	POINTER_DIRECTION_UP = 0,
	POINTER_DIRECTION_RIGHT = 1
};

void ui_shapes_circle(float centerX, float centerY, float radius, u32 color, float depth);
void ui_shapes_round_rect(float x, float y, float width, float height, float radius, u32 color, float depth);

// A rectangle whose edge pixels get partial coverage at fractional coordinates.
void ui_shapes_rect(float x, float y, float width, float height, u32 color, float depth);

// A rectangle outline `thickness` pixels wide, inside the box.
void ui_shapes_frame(float x, float y, float width, float height, float thickness, u32 color, float depth);

void ui_shapes_triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 color, float depth);

// A small triangle around the centre that points in a POINTER_DIRECTION_* direction.
void ui_shapes_pointer(float centerX, float centerY, float size, int direction, u32 color, float depth);

// A ring segment. Angles are in radians, clockwise from 12 o'clock.
void ui_shapes_arc(float centerX, float centerY, float radius, float from, float to, float thickness,
                   u32 color, float depth);
