// The live view pipeline: the preview worker that decodes and analyses frames, the staging buffer and the
// textures.
#pragma once
#include <3ds.h>
#include <citro2d.h>

#define PREVIEW_HISTOGRAM_BINS 256

// One pixel of the staging buffer and the view textures.
#define RGBA(red, green, blue) (((u32)(red) << 24) | ((u32)(green) << 16) | ((u32)(blue) << 8) | 0xFF)

extern int g_previewWidth; // the size of the frame on the view texture
extern int g_previewHeight;
extern int g_frameRowRepeat; // screen rows per frame row: 2 for the fast live view's half-height frames
extern bool g_haveFrame;
extern u32 g_histogram[PREVIEW_HISTOGRAM_BINS]; // luma histogram of the last live frame

// Allocates the staging buffer and starts the worker. False without the buffer: nothing can be shown.
bool preview_init(void);
// Stops the worker and frees the buffer and the textures.
void preview_exit(void);

// Makes the view texture big enough for a frame. Its sides are powers of two.
void preview_ensure_texture(int width, int height);
// The view texture. The right eye's texture holds the right eye of a 3D picture in the gallery; it is the
// size of the view texture.
C3D_Tex *preview_view_texture(void);
C3D_Tex *preview_right_eye_texture(void);

// Row y of the staging buffer: RGBA8 in linear memory, as many pixels per row as the view texture is wide.
// RGB565 would posterise dark gradients.
u32 *preview_staging_row(int y);
int preview_staging_stride(void);

// Decodes a camera frame (format: CAM_FORMAT_*) into the staging buffer, stride pixels per row. live: also
// draws zebra and peaking, counts the histogram, and feeds the meter, the white balance and the hot pixel
// learner. A review frame (an earlier photo) must not move them.
void preview_process(const void *frame, int format, int width, int height, int stride, bool live);

// Copy the staging buffer into the right eye's texture or the view texture.
void preview_upload_right_eye(int width, int height);
void preview_upload(int width, int height);

// Waits for the frame in flight. Any thread may call it.
void preview_drain(void);

// Waits for the frame in flight and drops it. For callers about to use the staging buffer or reset the
// camera buffers.
void preview_idle(void);

// Main loop: puts the frame the worker finished on screen (or drops it while the live view is hidden), then
// hands the newest camera frame to the worker. live: the live view is on screen.
void preview_feed_from_camera(bool live);

// The last camera frame the main loop took, for the look editor's test render. preview_last_frame returns
// NULL while there is none.
const void *preview_last_frame(int *width, int *height, int *format);
