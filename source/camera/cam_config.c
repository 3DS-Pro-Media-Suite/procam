#include "cam_config.h"
#include <3ds.h>
#include "cam_settings.h"
#include "cam_state.h"

// The raw readout is 648 x 488: the 640 x 480 active area with a border of 4 pixels on each side.
#define RAW_BORDER_PIXELS 4
// The fast live view reads half the rows, so the top border is half as high.
#define RAW_FAST_BORDER_ROWS (RAW_BORDER_PIXELS / 2)

// Trimming offsets stay even so the Bayer order stays RGGB.
static int round_down_to_even(int value)
{
	return value & ~1;
}

// The YUV stream is the whole frame: no trimming.
static void disable_trimming(u32 port, Result *firstError)
{
	Result err = *firstError;
	CAM_TRY(CAMU_SetTrimming(port, false));
	*firstError = err;
}

// Without this trimming the raw frame fits no receive the camera service allows.
void cam_config_raw_trim(u32 port, int width, int height, bool fastView, Result *firstError)
{
	Result err = *firstError;
	int activeRows = fastView ? CAM_FRAME_HEIGHT / 2 : CAM_FRAME_HEIGHT;
	int borderRows = fastView ? RAW_FAST_BORDER_ROWS : RAW_BORDER_PIXELS;
	int left = RAW_BORDER_PIXELS + round_down_to_even((CAM_FRAME_WIDTH - width) / 2);
	int top = borderRows + round_down_to_even((activeRows - height) / 2);
	CAM_TRY(CAMU_SetTrimming(port, true));
	CAM_TRY_AS("CAMU_SetTrimmingParams(port, x0, y0, x0 + w, y0 + h)",
	           CAMU_SetTrimmingParams(port, left, top, left + width, top + height));
	*firstError = err;
}

void cam_config_stream_trim(Result *firstError)
{
	if (g_camera.rawActive) {
		cam_config_raw_trim(g_camera.livePort, g_camera.frameWidth, g_camera.frameHeight, g_camera.fastViewOn,
		                    firstError);
	} else {
		disable_trimming(g_camera.livePort, firstError);
	}
}

void cam_config_stream_transfer(Result *firstError)
{
	cam_config_stream_trim(firstError);
	Result err = *firstError;
	u32 maxBytes = 0;
	CAM_TRY_AS("CAMU_GetMaxBytes(&maxBytes, S.w, S.h)",
	           CAMU_GetMaxBytes(&maxBytes, g_camera.frameWidth, g_camera.frameHeight));
	g_camera.transferBytes = maxBytes;
	CAM_TRY_AS("CAMU_SetTransferBytes(S.port, maxBytes, S.w, S.h)",
	           CAMU_SetTransferBytes(g_camera.livePort, maxBytes, g_camera.frameWidth, g_camera.frameHeight));
	*firstError = err;
}

static void apply_metering_window(u32 cameraSelect, int windowIndex, bool forAutoExposure, Result *firstError)
{
	Result err = *firstError;
	const CamMeteringWindow *window = &CAM_METERING_WINDOWS[windowIndex];
	if (forAutoExposure) {
		CAM_TRY_AS("CAMU_SetAutoExposureWindow(sel, w[0], w[1], w[2], w[3])",
		           CAMU_SetAutoExposureWindow(cameraSelect, window->x, window->y, window->width,
		                                      window->height));
	} else {
		CAM_TRY_AS("CAMU_SetAutoWhiteBalanceWindow(sel, w[0], w[1], w[2], w[3])",
		           CAMU_SetAutoWhiteBalanceWindow(cameraSelect, window->x, window->y, window->width,
		                                          window->height));
	}
	*firstError = err;
}

// Only the controls that differ from `previous` are pushed. At a start `previous` holds the defaults, so a
// plain start leaves the sensor's own tuning alone.
Result cam_config_apply_image_controls(u32 cameraSelect, const CamSettings *settings,
                                       const CamSettings *previous)
{
	Result err = 0;
#define CHANGED(field) (previous->field != settings->field)
	if (CHANGED(photoMode)) {
		// First: a photo mode may preset other controls.
		CAM_TRY_AS("CAMU_SetPhotoMode(sel, s->photoMode)",
		           CAMU_SetPhotoMode(cameraSelect, settings->photoMode));
	}
	if (CHANGED(frameRate)) {
		CAM_TRY_AS("CAMU_SetFrameRate(sel, s->frameRate)",
		           CAMU_SetFrameRate(cameraSelect, settings->frameRate));
	}
	if (CHANGED(noiseFilter)) {
		CAM_TRY_AS("CAMU_SetNoiseFilter(sel, s->noiseFilter)",
		           CAMU_SetNoiseFilter(cameraSelect, settings->noiseFilter));
	}
	if (CHANGED(lensCorrection)) {
		CAM_TRY_AS("CAMU_SetLensCorrection(sel, s->lensCorrection)",
		           CAMU_SetLensCorrection(cameraSelect, settings->lensCorrection));
	}
	if (CHANGED(sharpness)) {
		CAM_TRY_AS("CAMU_SetSharpness(sel, s->sharpness)",
		           CAMU_SetSharpness(cameraSelect, settings->sharpness));
	}
	if (CHANGED(contrast)) {
		CAM_TRY_AS("CAMU_SetContrast(sel, s->contrast)", CAMU_SetContrast(cameraSelect, settings->contrast));
	}
	if (CHANGED(aeWindow)) {
		apply_metering_window(cameraSelect, settings->aeWindow, true, &err);
	}
	if (CHANGED(awbWindow)) {
		apply_metering_window(cameraSelect, settings->awbWindow, false, &err);
	}
	if (CHANGED(exposure)) {
		CAM_TRY_AS("CAMU_SetExposure(sel, s->exposure)", CAMU_SetExposure(cameraSelect, settings->exposure));
	}
	// Also after a SetExposure under an AE lock: it switches the camera's AE back on.
	if (CHANGED(autoExposure) || (CHANGED(exposure) && !settings->autoExposure)) {
		CAM_TRY_AS("CAMU_SetAutoExposure(sel, s->autoExposure)",
		           CAMU_SetAutoExposure(cameraSelect, settings->autoExposure));
	}
	if (CHANGED(whiteBalance) || CHANGED(awbLock)) {
		// The preset goes before the auto flag, so a preset cannot switch auto back on.
		CAM_TRY_AS("CAMU_SetWhiteBalance(sel, s->whiteBalance)",
		           CAMU_SetWhiteBalance(cameraSelect, settings->whiteBalance));
		bool autoWhiteBalance = settings->whiteBalance == WHITE_BALANCE_AUTO && !settings->awbLock;
		CAM_TRY_AS("CAMU_SetAutoWhiteBalance(sel, s->whiteBalance == 0 && !s->awbLock)",
		           CAMU_SetAutoWhiteBalance(cameraSelect, autoWhiteBalance));
	}
	if (CHANGED(effect)) {
		CAM_TRY_AS("CAMU_SetEffect(sel, s->effect, CONTEXT_A)",
		           CAMU_SetEffect(cameraSelect, settings->effect, CONTEXT_A));
		CAM_TRY_AS("CAMU_SetEffect(sel, s->effect, CONTEXT_B)",
		           CAMU_SetEffect(cameraSelect, settings->effect, CONTEXT_B));
	}
	if (CHANGED(flip) && !g_camera.rawMode) {
		CAM_TRY_AS("CAMU_FlipImage(sel, s->flip, CONTEXT_A)",
		           CAMU_FlipImage(cameraSelect, settings->flip, CONTEXT_A));
		CAM_TRY_AS("CAMU_FlipImage(sel, s->flip, CONTEXT_B)",
		           CAMU_FlipImage(cameraSelect, settings->flip, CONTEXT_B));
	}
#undef CHANGED
	return err;
}
