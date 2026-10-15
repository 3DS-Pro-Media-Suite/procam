#include "preview.h"
#include <3ds.h>
#include <citro2d.h>
#include <stdlib.h>
#include <string.h>
#include "app_threads.h"
#include "background_merge.h"
#include "cam_settings.h"
#include "cam_stream.h"
#include "camera_control.h"
#include "fast_view.h"
#include "hot_pixel_live.h"
#include "metering.h"
#include "raw_grid.h"
#include "raw_view.h"
#include "settings.h"
#include "white_balance.h"

#define MIN_TEXTURE_SIDE 8 // the GPU's smallest texture side
#define BYTES_PER_PIXEL 4 // RGBA8
#define TRANSFER_FLAGS                                                                              \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) |                \
	 GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
	 GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

// The staging buffer holds the largest view texture.
#define STAGING_BUFFER_WIDTH 1024
#define STAGING_BUFFER_HEIGHT 512
#define STAGING_BUFFER_BYTES (STAGING_BUFFER_WIDTH * STAGING_BUFFER_HEIGHT * BYTES_PER_PIXEL)

// Zebra: diagonal stripes, 8 pixels wide, over blown highlights and crushed shadows.
#define ZEBRA_STRIPE_SHIFT 3
#define ZEBRA_HIGHLIGHT_LUMA 245
#define ZEBRA_SHADOW_LUMA 8
#define ZEBRA_HIGHLIGHT_COLOR RGBA(255, 0, 0)
#define ZEBRA_SHADOW_COLOR RGBA(0, 64, 255)
// Peaking: a luma step this large to the next pixel marks a sharp edge.
#define PEAKING_STEP 24
#define PEAKING_COLOR RGBA(0, 255, 0)

#define FAST_FRAME_ROW_REPEAT 2 // fast frames are half height: each row is shown twice

#define WORKER_STACK_SIZE (16 * 1024)
#define WORKER_POLL_NANOSECONDS 1000000LL // a millisecond

static u32 *g_stagingBuffer;
static C3D_Tex g_viewTexture;
static int g_viewTextureWidth;
static int g_viewTextureHeight;
static C3D_Tex g_rightEyeTexture;
static int g_rightEyeTextureWidth;
static int g_rightEyeTextureHeight;
int g_previewWidth;
int g_previewHeight;
int g_frameRowRepeat = 1;
bool g_haveFrame;
u32 g_histogram[PREVIEW_HISTOGRAM_BINS];
static u32 g_frameTick;
static u32 g_histogramCounts[PREVIEW_HISTOGRAM_BINS]; // filled while a frame is processed
static DecodedRow g_decodedRow;
static RawGrid g_rawGrid; // the grid of the last live raw frame

// The preview worker decodes the camera frames, on the New 3DS's third core when there is one, so the main
// thread keeps drawing at full rate. Only the main thread uploads, because citro3d is single-threaded. One
// frame is in flight at a time: the main thread takes no new camera frame while the worker is busy, so the
// buffer the worker reads cannot be refilled under it.
typedef struct {
	Thread thread;
	LightEvent wakeEvent;
	_Atomic bool busy;
	_Atomic bool done;
	_Atomic bool quit;
	const void *source;
	int format;
	int width;
	int height;
	int stride;
	int rowRepeat; // screen rows per frame row
} PreviewWorker;

static PreviewWorker g_previewWorker;

static const void *g_lastFramePixels;
static int g_lastFrameWidth;
static int g_lastFrameHeight;
static int g_lastFrameFormat;

// The smallest power of two texture side that holds length pixels.
static int texture_side(int length)
{
	int side = MIN_TEXTURE_SIDE;
	while (side < length) {
		side *= 2;
	}
	return side;
}

// Nearest filtering: the screen shows the camera's pixels unsmoothed.
static void create_texture(C3D_Tex *texture, int width, int height)
{
	C3D_TexInit(texture, width, height, GPU_RGBA8);
	C3D_TexSetFilter(texture, GPU_NEAREST, GPU_NEAREST);
	C3D_TexSetWrap(texture, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
}

void preview_ensure_texture(int width, int height)
{
	int textureWidth = texture_side(width);
	int textureHeight = texture_side(height);
	if (textureWidth == g_viewTextureWidth && textureHeight == g_viewTextureHeight) {
		return;
	}
	if (g_viewTextureWidth) {
		C3D_TexDelete(&g_viewTexture);
	}
	create_texture(&g_viewTexture, textureWidth, textureHeight);
	g_viewTextureWidth = textureWidth;
	g_viewTextureHeight = textureHeight;
}

C3D_Tex *preview_view_texture(void)
{
	return &g_viewTexture;
}

C3D_Tex *preview_right_eye_texture(void)
{
	return &g_rightEyeTexture;
}

u32 *preview_staging_row(int y)
{
	return g_stagingBuffer + y * g_viewTextureWidth;
}

int preview_staging_stride(void)
{
	return g_viewTextureWidth;
}

// Live raw frames feed the white balance, the meter and the hot pixel learner.
static void analyse_raw_frame(const void *frame, int width, int height)
{
	raw_grid_sample(&g_rawGrid, frame, width, height);
	hot_pixel_live_update((u8 *)frame, width, height);
	metering_update(frame, width, height, &g_rawGrid);
	white_balance_update(&g_rawGrid);
}

static void count_histogram(const u8 *luma, int width)
{
	for (int x = 0; x < width; x++) {
		g_histogramCounts[luma[x]]++;
	}
}

// The common case, kept tight: it runs for every preview pixel.
static void draw_row(u32 *destination, const DecodedRow *row, int width)
{
	for (int x = 0; x < width; x++) {
		destination[x] = RGBA(row->red[x], row->green[x], row->blue[x]);
	}
}

// The zebra stripes move one pixel every other frame.
static bool zebra_stripe(int x, int y)
{
	int diagonal = x + y + (int)(g_frameTick / 2);
	return (diagonal >> ZEBRA_STRIPE_SHIFT) & 1;
}

static void draw_row_with_overlays(u32 *destination, const DecodedRow *row, int width, int y, bool zebra,
                                   bool peaking)
{
	for (int x = 0; x < width; x++) {
		int luma = row->luma[x];
		u32 color = RGBA(row->red[x], row->green[x], row->blue[x]);
		if (zebra && zebra_stripe(x, y)) {
			if (luma >= ZEBRA_HIGHLIGHT_LUMA) {
				color = ZEBRA_HIGHLIGHT_COLOR;
			} else if (luma <= ZEBRA_SHADOW_LUMA) {
				color = ZEBRA_SHADOW_COLOR;
			}
		}
		if (peaking && abs(luma - row->luma[x + 1]) > PEAKING_STEP) {
			color = PEAKING_COLOR;
		}
		destination[x] = color;
	}
}

void preview_process(const void *frame, int format, int width, int height, int stride, bool live)
{
	bool countHistogram = live && g_settings.histogram;
	bool zebra = live && g_settings.zebra;
	bool peaking = live && g_settings.peaking;
	if (countHistogram) {
		memset(g_histogramCounts, 0, sizeof g_histogramCounts);
	}
	g_frameTick++;
	if (format == CAM_FORMAT_RAW) {
		raw_view_update_lut();
	}
	if (format == CAM_FORMAT_RAW && live) {
		analyse_raw_frame(frame, width, height);
	}
	DecodedRow *row = &g_decodedRow;
	for (int y = 0; y < height; y++) {
		bool evenRow = (y & 1) == 0;
		// Both rows of a raw cell decode the same.
		if (format != CAM_FORMAT_RAW || evenRow) {
			raw_view_decode_row(frame, format, width, y, 0, width, row->red, row->green, row->blue,
			                    row->luma);
		}
		row->luma[width] = row->luma[width - 1];
		// Every other row is enough for the histogram.
		if (countHistogram && evenRow) {
			count_histogram(row->luma, width);
		}
		u32 *destination = g_stagingBuffer + y * stride;
		if (zebra || peaking) {
			draw_row_with_overlays(destination, row, width, y, zebra, peaking);
		} else {
			draw_row(destination, row, width);
		}
	}
	// In one go: the main thread draws g_histogram meanwhile.
	if (countHistogram) {
		memcpy(g_histogram, g_histogramCounts, sizeof g_histogram);
	}
}

// The texture is the size of g_viewTexture. Rows below height are never drawn, so they are not flushed.
static void copy_staging_buffer_to(C3D_Tex *texture, int height)
{
	GSPGPU_FlushDataCache(g_stagingBuffer, g_viewTextureWidth * height * BYTES_PER_PIXEL);
	u32 size = GX_BUFFER_DIM(g_viewTextureWidth, g_viewTextureHeight);
	C3D_SyncDisplayTransfer(g_stagingBuffer, size, (u32 *)texture->data, size, TRANSFER_FLAGS);
}

void preview_upload_right_eye(int width, int height)
{
	preview_ensure_texture(width, height);
	if (g_rightEyeTextureWidth != g_viewTextureWidth || g_rightEyeTextureHeight != g_viewTextureHeight) {
		if (g_rightEyeTextureWidth) {
			C3D_TexDelete(&g_rightEyeTexture);
		}
		create_texture(&g_rightEyeTexture, g_viewTextureWidth, g_viewTextureHeight);
		g_rightEyeTextureWidth = g_viewTextureWidth;
		g_rightEyeTextureHeight = g_viewTextureHeight;
	}
	copy_staging_buffer_to(&g_rightEyeTexture, height);
}

void preview_upload(int width, int height)
{
	preview_ensure_texture(width, height);
	copy_staging_buffer_to(&g_viewTexture, height);
	g_previewWidth = width;
	g_previewHeight = height;
	g_frameRowRepeat = 1; // the live view sets its own after uploading a fast frame
	g_haveFrame = true;
}

static void run_preview_worker(void *argument)
{
	(void)argument;
	for (;;) {
		LightEvent_Wait(&g_previewWorker.wakeEvent);
		if (g_previewWorker.quit) {
			break;
		}
		preview_process(g_previewWorker.source, g_previewWorker.format, g_previewWorker.width,
		                g_previewWorker.height, g_previewWorker.stride, true);
		g_previewWorker.done = true;
		g_previewWorker.busy = false;
	}
}

static void start_worker(void)
{
	LightEvent_Init(&g_previewWorker.wakeEvent, RESET_ONESHOT);
	s32 priority = MAIN_THREAD_PRIORITY;
	svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
	// New 3DS: the third core, above other work there, so the viewfinder comes first.
	g_previewWorker.thread =
	        threadCreate(run_preview_worker, NULL, WORKER_STACK_SIZE, priority, THREAD_CORE_NEW3DS, false);
	if (g_previewWorker.thread) {
		return;
	}
	// Old 3DS: below the main thread, so the UI stays smooth and the preview takes what is left.
	g_previewWorker.thread =
	        threadCreate(run_preview_worker, NULL, WORKER_STACK_SIZE, priority + 1, THREAD_CORE_APP, false);
}

bool preview_init(void)
{
	g_stagingBuffer = linearAlloc(STAGING_BUFFER_BYTES);
	start_worker();
	return g_stagingBuffer != NULL;
}

static void stop_worker(void)
{
	if (!g_previewWorker.thread) {
		return;
	}
	preview_idle();
	g_previewWorker.quit = true;
	LightEvent_Signal(&g_previewWorker.wakeEvent);
	threadJoin(g_previewWorker.thread, U64_MAX);
	threadFree(g_previewWorker.thread);
	g_previewWorker.thread = NULL;
}

void preview_exit(void)
{
	stop_worker();
	if (g_viewTextureWidth) {
		C3D_TexDelete(&g_viewTexture);
		g_viewTextureWidth = 0;
		g_viewTextureHeight = 0;
	}
	if (g_rightEyeTextureWidth) {
		C3D_TexDelete(&g_rightEyeTexture);
		g_rightEyeTextureWidth = 0;
		g_rightEyeTextureHeight = 0;
	}
	linearFree(g_stagingBuffer);
	g_stagingBuffer = NULL;
}

void preview_drain(void)
{
	while (g_previewWorker.busy) {
		svcSleepThread(WORKER_POLL_NANOSECONDS);
	}
}

void preview_idle(void)
{
	preview_drain();
	g_previewWorker.done = false;
}

// Hands a camera frame to the preview worker, or processes it at once when there is no worker.
static void submit_frame(const void *frame, int format, int width, int height)
{
	if (!g_previewWorker.thread) {
		preview_ensure_texture(width, height);
		preview_process(frame, format, width, height, g_viewTextureWidth, true);
		preview_upload(width, height);
		return;
	}
	g_previewWorker.source = frame;
	g_previewWorker.format = format;
	g_previewWorker.width = width;
	bool fastFrame = format == CAM_FORMAT_RAW && fast_view_active();
	g_previewWorker.rowRepeat = fastFrame ? FAST_FRAME_ROW_REPEAT : 1;
	g_previewWorker.height = height;
	// The upload resizes the texture with this frame. Until then the last frame stays drawable.
	g_previewWorker.stride = texture_side(width);
	g_previewWorker.done = false;
	g_previewWorker.busy = true;
	LightEvent_Signal(&g_previewWorker.wakeEvent);
}

static bool preview_busy(void)
{
	return g_previewWorker.busy;
}

// Puts the frame the worker finished on screen when show is set, else drops it.
static void present_frame(bool show)
{
	if (!g_previewWorker.done) {
		return;
	}
	g_previewWorker.done = false;
	if (show) {
		preview_upload(g_previewWorker.width, g_previewWorker.height);
		g_frameRowRepeat = g_previewWorker.rowRepeat;
	}
}

// While a merge runs, only every 2nd live frame goes to the preview worker. Both share a core and the worker
// has the higher priority, so a full-rate live view would take much of the merge's time.
static bool should_submit_frame(bool live)
{
	static u32 framesDuringMerge;
	bool merging = background_merge_active();
	if (!live) {
		return false;
	}
	if (!merging) {
		return true;
	}
	framesDuringMerge++;
	return (framesDuringMerge & 1) != 0;
}

void preview_feed_from_camera(bool live)
{
	if (preview_busy()) {
		return;
	}
	present_frame(live);
	// No camera change can start between this check and the hand-off.
	camera_control_lock();
	u16 *pixels;
	int width;
	int height;
	if (camera_control_ok() && !camera_control_changing() && cam_stream_poll(&pixels, &width, &height)) {
		int format = cam_stream_format();
		g_lastFramePixels = pixels;
		g_lastFrameWidth = width;
		g_lastFrameHeight = height;
		g_lastFrameFormat = format;
		if (should_submit_frame(live)) {
			submit_frame(pixels, format, width, height);
		}
	}
	camera_control_unlock();
}

const void *preview_last_frame(int *width, int *height, int *format)
{
	*width = g_lastFrameWidth;
	*height = g_lastFrameHeight;
	*format = g_lastFrameFormat;
	return g_lastFramePixels;
}
