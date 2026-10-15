// What goes beside the pixels: the sidecar text and the EXIF block.
#pragma once
#include <3ds.h>
#include <time.h>
#include "cam_settings.h"

#define PHOTO_METADATA_EXIF_MAX_SIZE 256 // photo_metadata_make_exif writes at most this many bytes

// Metadata written with a photo: PNG text chunks, the EXIF block and DNG tags.
typedef struct {
	const char *title; // the file name without extension, e.g. "IMG_0042"
	const char *camera; // which sensor
	const char *text; // the sidecar text, written as the Description
	const CamSettings *camSettings;
	int exposureBias; // EV
	time_t when; // the console clock (local time; the 3DS has no time zone)
	// Exposure time, 0 = unknown (the camera service does not report it for YUV shots).
	float exposureSeconds;
	int iso; // ISO equivalent of the analog gain, 0 = unknown
	bool raw; // a raw shot: Pro Cam's own auto white balance, whatever the white balance setting says
} PhotoMeta;

// The shooting settings as text for the .TXT sidecar and the Description, malloc'd. rawWhiteBalance: a raw
// shot's as-shot gains (R, G, B), or NULL for a YUV shot. note is appended when not NULL.
char *photo_metadata_sidecar_text(const CamSettings *settings, int exposureBias, const char *note,
                                  const float *rawWhiteBalance);
// The EXIF block (IFD0 and the EXIF IFD) as little-endian TIFF. Returns its size.
size_t photo_metadata_make_exif(u8 *out, const PhotoMeta *meta);
