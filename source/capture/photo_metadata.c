#include "photo_metadata.h"
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "cam_settings.h"
#include "settings.h"
#include "raw10.h"
#include "tiff_writer.h"

#define SIDECAR_SETTINGS_TEXT_SIZE 1024 // room for the settings lines, before the note

// The sidecar's own short value names, indexed by the matching CamSettings field. Saved files keep these
// whatever the menus call the values.
static const char *const SIDECAR_METERING_WINDOW_NAMES[CAM_METERING_WINDOW_COUNT] = {
        "Full", "Centre", "Spot", "Top", "Bottom", "Left", "Right"};
static const char *const SIDECAR_FLIP_NAMES[CAM_FLIP_COUNT] = {"None", "Horizontal", "Vertical", "Both"};
static const char *const SIDECAR_LENS_CORRECTION_NAMES[CAM_LENS_CORRECTION_COUNT] = {"Off", "70", "90"};
static const char *const SIDECAR_WHITE_BALANCE_NAMES[CAM_WHITE_BALANCE_COUNT] = {
        "Auto", "3200 K", "4150 K", "5200 K", "6000 K", "7000 K"};
static const char *const SIDECAR_PHOTO_MODE_NAMES[CAM_PHOTO_MODE_COUNT] = {"Normal", "Portrait", "Landscape",
                                                                           "Night", "Letter"};
static const char *const SIDECAR_EFFECT_NAMES[CAM_EFFECT_COUNT] = {"None",     "Mono",     "Sepia",
                                                                   "Negative", "Negafilm", "Sepia 2"};

// The EXIF block: the header, IFD0, the EXIF IFD, then the values that do not fit in their entries.
#define IFD0_ENTRY_COUNT 5
#define EXIF_IFD_ENTRY_COUNT 5
#define IFD_SIZE(entryCount) (TIFF_IFD_COUNT_SIZE + (entryCount) * TIFF_IFD_ENTRY_SIZE + TIFF_NEXT_IFD_SIZE)
#define IFD0_OFFSET TIFF_HEADER_SIZE
#define EXIF_IFD_OFFSET (IFD0_OFFSET + IFD_SIZE(IFD0_ENTRY_COUNT))
#define EXIF_VALUES_OFFSET (EXIF_IFD_OFFSET + IFD_SIZE(EXIF_IFD_ENTRY_COUNT))

enum {
	EXIF_TAG_EXIF_IFD_POINTER = 34665,
	EXIF_TAG_DATE_TIME_ORIGINAL = 36867,
	EXIF_TAG_EXPOSURE_BIAS = 37380,
	EXIF_TAG_WHITE_BALANCE = 41987,
};

#define EXIF_WHITE_BALANCE_AUTO 0
#define EXIF_WHITE_BALANCE_MANUAL 1

typedef struct {
	u8 *block; // the TIFF block being written
	u8 *nextEntry;
	u32 valuesEnd; // where the next value that does not fit in its entry goes
} ExifWriter;

char *photo_metadata_sidecar_text(const CamSettings *settings, int exposureBias, const char *note,
                                  const float *rawWhiteBalance)
{
	size_t capacity = SIDECAR_SETTINGS_TEXT_SIZE + (note ? strlen(note) : 0);
	char *text = malloc(capacity);
	if (!text) {
		return NULL;
	}
	int length = snprintf(
	        text, capacity, "camera: %s\nexposure bias: %+d EV%s\nauto exposure: %s (metering: %s)\n",
	        CAM_CAMERA_NAMES[(int)settings->camera], exposureBias,
	        exposureBias != settings->exposure ? " (bracketed)" : "",
	        settings->autoExposure ? "on" : "locked", SIDECAR_METERING_WINDOW_NAMES[(int)settings->aeWindow]);
	if (rawWhiteBalance) {
		// A raw frame is taken before the camera's image processing, so its image controls are not listed.
		length +=
		        snprintf(text + length, capacity - length,
		                 "white balance: auto (grey pixels), as-shot gains R %.3f G %.3f B %.3f\nflip: %s\n",
		                 rawWhiteBalance[RGB_RED], rawWhiteBalance[RGB_GREEN], rawWhiteBalance[RGB_BLUE],
		                 SIDECAR_FLIP_NAMES[(int)settings->flip]);
	} else {
		bool lockedAuto = settings->whiteBalance == WHITE_BALANCE_AUTO && settings->awbLock;
		length += snprintf(
		        text + length, capacity - length,
		        "white balance: %s%s (metering: %s)\nsharpness: %+d\ncontrast: %+d\nnoise filter: %s\n"
		        "lens correction: %s\n",
		        SIDECAR_WHITE_BALANCE_NAMES[(int)settings->whiteBalance], lockedAuto ? " (locked)" : "",
		        SIDECAR_METERING_WINDOW_NAMES[(int)settings->awbWindow], settings->sharpness,
		        settings->contrast - CONTRAST_NORMAL, settings->noiseFilter ? "on" : "off",
		        SIDECAR_LENS_CORRECTION_NAMES[(int)settings->lensCorrection]);
		length += snprintf(
		        text + length, capacity - length, "photo mode: %s\neffect: %s\nflip: %s\nframe rate: %s\n",
		        SIDECAR_PHOTO_MODE_NAMES[(int)settings->photoMode],
		        SIDECAR_EFFECT_NAMES[(int)settings->effect], SIDECAR_FLIP_NAMES[(int)settings->flip],
		        CAM_FRAME_RATE_NAMES[(int)settings->frameRate]);
	}
	if (note) {
		snprintf(text + length, capacity - length, "%s", note);
	}
	return text;
}

static u32 align_up(u32 value, u32 alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

static void begin_ifd(ExifWriter *writer, u32 ifdOffset, u16 entryCount)
{
	u8 *ifd = writer->block + ifdOffset;
	tiff_writer_put_u16(ifd, entryCount);
	writer->nextEntry = ifd + TIFF_IFD_COUNT_SIZE;
}

static void end_ifd(ExifWriter *writer)
{
	tiff_writer_put_u32(writer->nextEntry, 0); // no next IFD
}

static u8 *add_entry(ExifWriter *writer, u16 tag, u16 type, u32 count)
{
	u8 *entry = writer->nextEntry;
	tiff_writer_put_entry(entry, tag, type, count);
	writer->nextEntry += TIFF_IFD_ENTRY_SIZE;
	return entry;
}

// Reserves room for a value that does not fit in its entry and points the entry at it.
static u8 *add_outside_value(ExifWriter *writer, u8 *entry, u32 size)
{
	u8 *value = writer->block + writer->valuesEnd;
	tiff_writer_put_u32(entry + TIFF_ENTRY_VALUE_OFFSET, writer->valuesEnd);
	writer->valuesEnd += align_up(size, TIFF_VALUE_ALIGNMENT);
	return value;
}

static void add_text(ExifWriter *writer, u16 tag, const char *text, u32 size)
{
	u8 *entry = add_entry(writer, tag, TIFF_TYPE_ASCII, size);
	memcpy(add_outside_value(writer, entry, size), text, size);
}

static void add_short(ExifWriter *writer, u16 tag, u16 value)
{
	u8 *entry = add_entry(writer, tag, TIFF_TYPE_SHORT, 1);
	tiff_writer_put_u16(entry + TIFF_ENTRY_VALUE_OFFSET, value);
}

static void add_long(ExifWriter *writer, u16 tag, u32 value)
{
	u8 *entry = add_entry(writer, tag, TIFF_TYPE_LONG, 1);
	tiff_writer_put_u32(entry + TIFF_ENTRY_VALUE_OFFSET, value);
}

static void add_rational(ExifWriter *writer, u16 tag, u16 type, u32 numerator, u32 denominator)
{
	u8 *entry = add_entry(writer, tag, type, 1);
	u8 *value = add_outside_value(writer, entry, TIFF_RATIONAL_SIZE);
	tiff_writer_put_u32(value, numerator);
	tiff_writer_put_u32(value + sizeof(u32), denominator);
}

// Manual for a YUV shot with a preset or locked auto white balance. A raw shot always uses Pro Cam's auto.
static u16 exif_white_balance(const PhotoMeta *meta)
{
	if (meta->raw) {
		return EXIF_WHITE_BALANCE_AUTO;
	}
	const CamSettings *settings = meta->camSettings;
	bool unlockedAuto = settings->whiteBalance == WHITE_BALANCE_AUTO && !settings->awbLock;
	return unlockedAuto ? EXIF_WHITE_BALANCE_AUTO : EXIF_WHITE_BALANCE_MANUAL;
}

size_t photo_metadata_make_exif(u8 *out, const PhotoMeta *meta)
{
	char dateTime[TIFF_DATE_TIME_SIZE];
	tiff_writer_date_time(&meta->when, dateTime);
	static const char make[] = "Nintendo";
	static const char software[] = "Pro Cam";
	const char *model = g_isNew3ds ? "New Nintendo 3DS" : "Nintendo 3DS"; // the console it runs on
	memset(out, 0, PHOTO_METADATA_EXIF_MAX_SIZE);
	tiff_writer_put_header(out);
	ExifWriter writer = {.block = out, .nextEntry = NULL, .valuesEnd = EXIF_VALUES_OFFSET};

	begin_ifd(&writer, IFD0_OFFSET, IFD0_ENTRY_COUNT);
	add_text(&writer, TIFF_TAG_MAKE, make, sizeof make);
	add_text(&writer, TIFF_TAG_MODEL, model, (u32)strlen(model) + 1);
	add_text(&writer, TIFF_TAG_SOFTWARE, software, sizeof software);
	add_text(&writer, TIFF_TAG_DATE_TIME, dateTime, TIFF_DATE_TIME_SIZE);
	add_long(&writer, EXIF_TAG_EXIF_IFD_POINTER, EXIF_IFD_OFFSET);
	end_ifd(&writer);

	u32 exposureTime[2];
	tiff_writer_exposure_rational(meta->exposureSeconds, exposureTime);
	begin_ifd(&writer, EXIF_IFD_OFFSET, EXIF_IFD_ENTRY_COUNT);
	add_rational(&writer, TIFF_TAG_EXPOSURE_TIME, TIFF_TYPE_RATIONAL, exposureTime[0], exposureTime[1]);
	add_short(&writer, TIFF_TAG_ISO_SPEED_RATINGS, (u16)meta->iso);
	add_text(&writer, EXIF_TAG_DATE_TIME_ORIGINAL, dateTime, TIFF_DATE_TIME_SIZE);
	add_rational(&writer, EXIF_TAG_EXPOSURE_BIAS, TIFF_TYPE_SRATIONAL, (u32)meta->exposureBias, 1);
	add_short(&writer, EXIF_TAG_WHITE_BALANCE, exif_white_balance(meta));
	end_ifd(&writer);
	return writer.valuesEnd;
}
