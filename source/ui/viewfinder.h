// The top screen: live view or gallery picture with the HUD, histogram, mode badge and the toast message.
#pragma once
#include <3ds.h>

// Drawing depths of the top screen: a larger value is drawn on top.
#define DEPTH_PICTURE 0.5f
#define DEPTH_GALLERY_BLANK 0.55f
#define DEPTH_OVERLAY 0.6f // shades, grid and the histogram's background
#define DEPTH_HISTOGRAM_BARS 0.65f
#define DEPTH_HISTOGRAM_BASELINE 0.66f
#define DEPTH_METER_FRAME 0.66f
#define DEPTH_HUD 0.7f
#define DEPTH_HUD_BAR 0.71f
#define DEPTH_EV_POINTER 0.72f
#define DEPTH_TIMER_DISC 0.8f
#define DEPTH_TIMER_TRACK 0.81f
#define DEPTH_TIMER_RING 0.82f
#define DEPTH_PILL 0.85f
#define DEPTH_MERGE_TRACK 0.86f
#define DEPTH_MERGE_FILL 0.87f
#define DEPTH_HINTS 0.9f
#define DEPTH_FLASH 0.95f

// The strip along the top under a title.
#define TITLE_SHADE_HEIGHT 30
#define TOP_TEXT_Y 7
#define TITLE_SIDE_TEXT_Y 10 // small text in line with a title

// The viewfinder is laid out for the top screen. On the narrower touch screen (screens swapped) it is drawn
// shifted left, which drops only the black bars beside the 4:3 picture; the edge texts move in by the inset.
void viewfinder_set_inset(int inset);
// Where texts along the left and right edge start and end.
int viewfinder_left_edge(void);
int viewfinder_right_edge(void);
// A shade behind the top strip keeps its text readable on a bright picture.
void viewfinder_draw_top_shade(float height);

// eye 0 is the left (or only) eye. eye 1 is the right eye, drawn only while the gallery shows a 3D picture.
void viewfinder_draw(int eye);

// A capture in progress or settings being applied, over the live view.
void viewfinder_draw_camera_status(void);
