#include "dng_file.h"
#include <3ds.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "cam_settings.h"
#include "image_files.h"
#include "look.h"
#include "look_render.h"
#include "math_util.h"
#include "photo_metadata.h"
#include "raw10.h"
#include "raw_view.h"
#include "tiff_writer.h"

// A DNG holds the raw frame as the sensor sent it, as 16-bit RGGB samples in one uncompressed strip, with
// the metadata raw converters need.

#define BYTES_PER_SAMPLE 2
#define DNG_BITS_PER_SAMPLE 16
#define SAMPLES_ALIGNMENT 16 // the strip starts on a 16-byte boundary
#define CFA_SIZE 2 // the colour filter pattern repeats every 2 x 2 pixels

#define SINGULAR_DETERMINANT 1e-6f
#define COLOR_MATRIX_DENOMINATOR 10000
#define NEUTRAL_DENOMINATOR 1000000
#define BASELINE_EXPOSURE_DENOMINATOR 100 // BaselineExposure in 1/100 EV

#define MIN_FILE_SIZE 16
#define MAX_IMAGE_SIZE 8192 // largest width or height read back
#define MIN_WHITE_LEVEL 2
#define MIN_BASELINE_EV 0.005f // smaller values count as no BaselineExposure
#define MAX_BASELINE_EV 8 // larger values are not believed

#define FLIP_BITS (FLIP_HORIZONTAL | FLIP_VERTICAL)

enum {
	TIFF_TAG_NEW_SUBFILE_TYPE = 254,
	TIFF_TAG_IMAGE_WIDTH = 256,
	TIFF_TAG_IMAGE_LENGTH = 257,
	TIFF_TAG_BITS_PER_SAMPLE = 258,
	TIFF_TAG_COMPRESSION = 259,
	TIFF_TAG_PHOTOMETRIC_INTERPRETATION = 262,
	TIFF_TAG_IMAGE_DESCRIPTION = 270,
	TIFF_TAG_STRIP_OFFSETS = 273,
	TIFF_TAG_ORIENTATION = 274,
	TIFF_TAG_SAMPLES_PER_PIXEL = 277,
	TIFF_TAG_ROWS_PER_STRIP = 278,
	TIFF_TAG_STRIP_BYTE_COUNTS = 279,
	TIFF_TAG_PLANAR_CONFIGURATION = 284,
	TIFF_TAG_CFA_REPEAT_PATTERN_DIM = 33421,
	TIFF_TAG_CFA_PATTERN = 33422,
	DNG_TAG_VERSION = 50706,
	DNG_TAG_BACKWARD_VERSION = 50707,
	DNG_TAG_UNIQUE_CAMERA_MODEL = 50708,
	DNG_TAG_BLACK_LEVEL_REPEAT_DIM = 50713,
	DNG_TAG_BLACK_LEVEL = 50714,
	DNG_TAG_WHITE_LEVEL = 50717,
	DNG_TAG_COLOR_MATRIX_1 = 50721,
	DNG_TAG_AS_SHOT_NEUTRAL = 50728,
	DNG_TAG_BASELINE_EXPOSURE = 50730,
	DNG_TAG_CALIBRATION_ILLUMINANT_1 = 50778,
};

enum {
	SUBFILE_MAIN_IMAGE = 0,
	COMPRESSION_NONE = 1,
	PHOTOMETRIC_CFA = 32803,
	PLANAR_CHUNKY = 1,
	ILLUMINANT_D65 = 21,
};

enum {
	CFA_RED = 0,
	CFA_GREEN = 1,
	CFA_BLUE = 2,
};

enum {
	ORIENTATION_NORMAL = 1,
	ORIENTATION_MIRROR_HORIZONTAL = 2,
	ORIENTATION_ROTATE_180 = 3,
	ORIENTATION_MIRROR_VERTICAL = 4,
	ORIENTATION_COUNT = 9, // TIFF orientations are 1 to 8
};

static const u16 ORIENTATION_OF_FLIP[] = {
        [FLIP_NONE] = ORIENTATION_NORMAL,
        [FLIP_HORIZONTAL] = ORIENTATION_MIRROR_HORIZONTAL,
        [FLIP_VERTICAL] = ORIENTATION_MIRROR_VERTICAL,
        [FLIP_REVERSE] = ORIENTATION_ROTATE_180,
};

// Orientations Pro Cam does not write show unflipped.
static const u8 FLIP_OF_ORIENTATION[ORIENTATION_COUNT] = {
        [ORIENTATION_MIRROR_HORIZONTAL] = FLIP_HORIZONTAL,
        [ORIENTATION_ROTATE_180] = FLIP_REVERSE,
        [ORIENTATION_MIRROR_VERTICAL] = FLIP_VERTICAL,
};

// XYZ (D65) to linear sRGB.
static const float XYZ_TO_SRGB[COLOR_MATRIX_ENTRIES] = {
        3.2406f,  -1.5372f, -0.4986f, // red
        -0.9689f, 1.8758f,  0.0415f, // green
        0.0557f,  -0.2040f, 1.0570f, // blue
};

static const u16 CFA_REPEAT[] = {CFA_SIZE, CFA_SIZE};
static const u8 CFA_PATTERN_RGGB[CFA_CELLS] = {CFA_RED, CFA_GREEN, CFA_GREEN, CFA_BLUE};
static const u8 DNG_VERSION[] = {1, 4, 0, 0};
static const u8 DNG_BACKWARD_VERSION[] = {1, 1, 0, 0};
static const char CAMERA_MAKE[] = "Nintendo";
static const char CAMERA_MODEL[] = "3DS";
static const char UNIQUE_CAMERA_MODEL[] = "Nintendo 3DS MT9V113";
static const char SOFTWARE_NAME[] = "Pro Cam";

typedef struct {
	u16 tag;
	u16 type;
	u32 count;
	u32 value; // the value itself, or the file offset of the values stored outside the entry
	const void *data; // the values, or NULL when value holds the one number
} TiffEntry;

// What Pro Cam reads back from its own DNGs.
typedef struct {
	int width;
	int height;
	int orientation;
	int iso;
	u32 stripOffset;
	u32 whiteLevel;
	float blackLevels[CFA_CELLS];
	float neutral[RGB_CHANNELS];
	float exposureSeconds;
	float baselineEv;
} DngInfo;

typedef struct {
	u32 width;
	u32 height;
	u32 bitsPerSample;
} DngImageSize;

static size_t tiff_type_size(u16 type)
{
	switch (type) {
	case TIFF_TYPE_SHORT:
		return sizeof(u16);
	case TIFF_TYPE_LONG:
	case TIFF_TYPE_SLONG:
		return sizeof(u32);
	case TIFF_TYPE_RATIONAL:
	case TIFF_TYPE_SRATIONAL:
		return TIFF_RATIONAL_SIZE;
	default:
		return 1;
	}
}

static size_t entry_data_size(const TiffEntry *entry)
{
	return tiff_type_size(entry->type) * entry->count;
}

static bool stored_outside_entry(const TiffEntry *entry)
{
	return entry->data && entry_data_size(entry) > TIFF_INLINE_VALUE_SIZE;
}

static int compare_tags(const void *first, const void *second)
{
	return (int)((const TiffEntry *)first)->tag - (int)((const TiffEntry *)second)->tag;
}

static u32 align_up(u32 value, u32 alignment)
{
	return (value + alignment - 1) & ~(alignment - 1);
}

// The inverse by cofactors. False when the matrix is (nearly) singular.
static bool invert_3x3(const float *m, float *inverse)
{
	float determinant = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
	                    m[2] * (m[3] * m[7] - m[4] * m[6]);
	if (fabsf(determinant) < SINGULAR_DETERMINANT) {
		return false;
	}
	inverse[0] = (m[4] * m[8] - m[5] * m[7]) / determinant;
	inverse[1] = (m[2] * m[7] - m[1] * m[8]) / determinant;
	inverse[2] = (m[1] * m[5] - m[2] * m[4]) / determinant;
	inverse[3] = (m[5] * m[6] - m[3] * m[8]) / determinant;
	inverse[4] = (m[0] * m[8] - m[2] * m[6]) / determinant;
	inverse[5] = (m[2] * m[3] - m[0] * m[5]) / determinant;
	inverse[6] = (m[3] * m[7] - m[4] * m[6]) / determinant;
	inverse[7] = (m[1] * m[6] - m[0] * m[7]) / determinant;
	inverse[8] = (m[0] * m[4] - m[1] * m[3]) / determinant;
	return true;
}

static void multiply_3x3(const float *left, const float *right, float *product)
{
	for (int row = 0; row < RGB_CHANNELS; row++) {
		const float *leftRow = left + row * RGB_CHANNELS;
		for (int column = 0; column < RGB_CHANNELS; column++) {
			product[row * RGB_CHANNELS + column] = leftRow[0] * right[column] +
			                                       leftRow[1] * right[RGB_CHANNELS + column] +
			                                       leftRow[2] * right[2 * RGB_CHANNELS + column];
		}
	}
}

// ColorMatrix1 maps XYZ to camera RGB. With a calibration (white-balanced camera RGB to linear sRGB) it is
// inverse(calibration) x XYZ_TO_SRGB; without one, XYZ_TO_SRGB itself.
static void color_matrix_rationals(const float *calibration, s32 rationals[COLOR_MATRIX_ENTRIES * 2])
{
	float xyzToCamera[COLOR_MATRIX_ENTRIES];
	float inverse[COLOR_MATRIX_ENTRIES];
	if (calibration && invert_3x3(calibration, inverse)) {
		multiply_3x3(inverse, XYZ_TO_SRGB, xyzToCamera);
	} else {
		memcpy(xyzToCamera, XYZ_TO_SRGB, sizeof xyzToCamera);
	}
	for (int i = 0; i < COLOR_MATRIX_ENTRIES; i++) {
		rationals[i * 2] = (s32)lrintf(xyzToCamera[i] * COLOR_MATRIX_DENOMINATOR);
		rationals[i * 2 + 1] = COLOR_MATRIX_DENOMINATOR;
	}
}

// AsShotNeutral is the reciprocal of a white balance gain.
static u32 neutral_numerator(float gain)
{
	float usableGain = gain > 0 ? gain : 1;
	return (u32)(NEUTRAL_DENOMINATOR / usableGain);
}

// Leaves out the optional tags that would say nothing. Returns the number of entries kept.
static int drop_empty_entries(TiffEntry *entries, int count, bool exposureUnknown, bool hasBaseline)
{
	int kept = 0;
	for (int i = 0; i < count; i++) {
		u16 tag = entries[i].tag;
		bool exposureTag = tag == TIFF_TAG_EXPOSURE_TIME || tag == TIFF_TAG_ISO_SPEED_RATINGS;
		if (exposureTag && exposureUnknown) {
			continue;
		}
		if (tag == DNG_TAG_BASELINE_EXPOSURE && !hasBaseline) {
			continue;
		}
		entries[kept] = entries[i];
		kept++;
	}
	return kept;
}

// The file holds the header, the IFD, the values stored outside their entries, then the samples.
// Places the values and returns where the samples start.
static u32 lay_out_file(TiffEntry *entries, int count)
{
	u32 offset = TIFF_HEADER_SIZE + TIFF_IFD_COUNT_SIZE + TIFF_IFD_ENTRY_SIZE * count + TIFF_NEXT_IFD_SIZE;
	for (int i = 0; i < count; i++) {
		if (stored_outside_entry(&entries[i])) {
			entries[i].value = offset;
			offset += align_up(entry_data_size(&entries[i]), TIFF_VALUE_ALIGNMENT);
		}
	}
	u32 samplesOffset = align_up(offset, SAMPLES_ALIGNMENT);
	for (int i = 0; i < count; i++) {
		if (entries[i].tag == TIFF_TAG_STRIP_OFFSETS) {
			entries[i].value = samplesOffset;
		}
	}
	return samplesOffset;
}

static void write_entry(u8 *file, u8 *slot, const TiffEntry *entry)
{
	size_t size = entry_data_size(entry);
	u8 *valueField = slot + TIFF_ENTRY_VALUE_OFFSET;
	tiff_writer_put_entry(slot, entry->tag, entry->type, entry->count);
	if (stored_outside_entry(entry)) {
		tiff_writer_put_u32(valueField, entry->value);
		memcpy(file + entry->value, entry->data, size);
	} else if (entry->data) {
		memcpy(valueField, entry->data, size);
	} else if (entry->type == TIFF_TYPE_SHORT) {
		tiff_writer_put_u16(valueField, (u16)entry->value);
	} else {
		tiff_writer_put_u32(valueField, entry->value);
	}
}

static void write_header_and_ifd(u8 *file, const TiffEntry *entries, int count)
{
	tiff_writer_put_header(file);
	u8 *ifd = file + TIFF_HEADER_SIZE;
	tiff_writer_put_u16(ifd, (u16)count);
	u8 *slot = ifd + TIFF_IFD_COUNT_SIZE;
	for (int i = 0; i < count; i++) {
		write_entry(file, slot, &entries[i]);
		slot += TIFF_IFD_ENTRY_SIZE;
	}
	tiff_writer_put_u32(slot, 0); // no next IFD
}

// The samples as 16-bit little-endian values.
static void write_samples(u8 *destination, const u16 *samples, u32 sampleCount)
{
	for (u32 i = 0; i < sampleCount; i++) {
		u8 *sample = destination + i * BYTES_PER_SAMPLE;
		sample[0] = (u8)samples[i];
		sample[1] = (u8)(samples[i] >> 8);
	}
}

static bool write_dng(const char *path, TiffEntry *entries, int count, const u16 *samples, u32 sampleCount)
{
	qsort(entries, count, sizeof entries[0], compare_tags); // TIFF wants the entries in tag order
	u32 samplesOffset = lay_out_file(entries, count);
	u32 fileSize = samplesOffset + sampleCount * BYTES_PER_SAMPLE;
	u8 *file = calloc(1, fileSize);
	if (!file) {
		return false;
	}
	write_header_and_ifd(file, entries, count);
	write_samples(file + samplesOffset, samples, sampleCount);
	bool saved = image_files_save_bytes(path, file, fileSize);
	free(file);
	return saved;
}

bool dng_file_save_mosaic(const char *path, const u16 *samples, int width, int height, int white, int flip,
                          const float *whiteBalance, const float *blackLevels, const PhotoMeta *meta,
                          const float *calibration, float baselineEv)
{
	s32 colorMatrix[COLOR_MATRIX_ENTRIES * 2];
	color_matrix_rationals(calibration, colorMatrix);
	char dateTime[TIFF_DATE_TIME_SIZE];
	tiff_writer_date_time(meta ? &meta->when : NULL, dateTime);
	// One black level per CFA cell: some converters reject a 2 x 2 BlackLevelRepeatDim with a single value.
	u32 black[CFA_CELLS];
	for (int cell = 0; cell < CFA_CELLS; cell++) {
		black[cell] = (u32)(blackLevels[cell] + 0.5f);
	}
	// AsShotNeutral: numerator and denominator per channel.
	u32 neutral[RGB_CHANNELS * 2] = {
	        neutral_numerator(whiteBalance[0]),
	        NEUTRAL_DENOMINATOR, // red
	        NEUTRAL_DENOMINATOR,
	        NEUTRAL_DENOMINATOR, // green
	        neutral_numerator(whiteBalance[2]),
	        NEUTRAL_DENOMINATOR, // blue
	};
	u32 exposureTime[2];
	tiff_writer_exposure_rational(meta ? meta->exposureSeconds : 0, exposureTime);
	s32 baseline[2] = {(s32)lrintf(baselineEv * BASELINE_EXPOSURE_DENOMINATOR),
	                   BASELINE_EXPOSURE_DENOMINATOR};
	const char *description = meta && meta->text ? meta->text : "";
	u32 iso = meta ? (u16)meta->iso : 0;
	u32 sampleBytes = (u32)width * height * BYTES_PER_SAMPLE;
	TiffEntry entries[] = {
	        {TIFF_TAG_NEW_SUBFILE_TYPE, TIFF_TYPE_LONG, 1, SUBFILE_MAIN_IMAGE, NULL},
	        {TIFF_TAG_IMAGE_WIDTH, TIFF_TYPE_LONG, 1, (u32)width, NULL},
	        {TIFF_TAG_IMAGE_LENGTH, TIFF_TYPE_LONG, 1, (u32)height, NULL},
	        {TIFF_TAG_BITS_PER_SAMPLE, TIFF_TYPE_SHORT, 1, DNG_BITS_PER_SAMPLE, NULL},
	        {TIFF_TAG_COMPRESSION, TIFF_TYPE_SHORT, 1, COMPRESSION_NONE, NULL},
	        {TIFF_TAG_PHOTOMETRIC_INTERPRETATION, TIFF_TYPE_SHORT, 1, PHOTOMETRIC_CFA, NULL},
	        {TIFF_TAG_IMAGE_DESCRIPTION, TIFF_TYPE_ASCII, (u32)strlen(description) + 1, 0, description},
	        {TIFF_TAG_MAKE, TIFF_TYPE_ASCII, sizeof CAMERA_MAKE, 0, CAMERA_MAKE},
	        {TIFF_TAG_MODEL, TIFF_TYPE_ASCII, sizeof CAMERA_MODEL, 0, CAMERA_MODEL},
	        {TIFF_TAG_STRIP_OFFSETS, TIFF_TYPE_LONG, 1, 0, NULL},
	        {TIFF_TAG_ORIENTATION, TIFF_TYPE_SHORT, 1, ORIENTATION_OF_FLIP[flip & FLIP_BITS], NULL},
	        {TIFF_TAG_SAMPLES_PER_PIXEL, TIFF_TYPE_SHORT, 1, 1, NULL},
	        {TIFF_TAG_ROWS_PER_STRIP, TIFF_TYPE_LONG, 1, (u32)height, NULL},
	        {TIFF_TAG_STRIP_BYTE_COUNTS, TIFF_TYPE_LONG, 1, sampleBytes, NULL},
	        {TIFF_TAG_PLANAR_CONFIGURATION, TIFF_TYPE_SHORT, 1, PLANAR_CHUNKY, NULL},
	        {TIFF_TAG_SOFTWARE, TIFF_TYPE_ASCII, sizeof SOFTWARE_NAME, 0, SOFTWARE_NAME},
	        {TIFF_TAG_DATE_TIME, TIFF_TYPE_ASCII, TIFF_DATE_TIME_SIZE, 0, dateTime},
	        {TIFF_TAG_CFA_REPEAT_PATTERN_DIM, TIFF_TYPE_SHORT, ARRAY_COUNT(CFA_REPEAT), 0, CFA_REPEAT},
	        {TIFF_TAG_CFA_PATTERN, TIFF_TYPE_BYTE, ARRAY_COUNT(CFA_PATTERN_RGGB), 0, CFA_PATTERN_RGGB},
	        {DNG_TAG_VERSION, TIFF_TYPE_BYTE, ARRAY_COUNT(DNG_VERSION), 0, DNG_VERSION},
	        {DNG_TAG_BACKWARD_VERSION, TIFF_TYPE_BYTE, ARRAY_COUNT(DNG_BACKWARD_VERSION), 0,
	         DNG_BACKWARD_VERSION},
	        {DNG_TAG_UNIQUE_CAMERA_MODEL, TIFF_TYPE_ASCII, sizeof UNIQUE_CAMERA_MODEL, 0,
	         UNIQUE_CAMERA_MODEL},
	        {DNG_TAG_BLACK_LEVEL_REPEAT_DIM, TIFF_TYPE_SHORT, ARRAY_COUNT(CFA_REPEAT), 0, CFA_REPEAT},
	        {DNG_TAG_BLACK_LEVEL, TIFF_TYPE_LONG, CFA_CELLS, 0, black},
	        {DNG_TAG_WHITE_LEVEL, TIFF_TYPE_LONG, 1, (u32)white, NULL},
	        {DNG_TAG_COLOR_MATRIX_1, TIFF_TYPE_SRATIONAL, COLOR_MATRIX_ENTRIES, 0, colorMatrix},
	        {DNG_TAG_AS_SHOT_NEUTRAL, TIFF_TYPE_RATIONAL, RGB_CHANNELS, 0, neutral},
	        {DNG_TAG_BASELINE_EXPOSURE, TIFF_TYPE_SRATIONAL, 1, 0, baseline},
	        {DNG_TAG_CALIBRATION_ILLUMINANT_1, TIFF_TYPE_SHORT, 1, ILLUMINANT_D65, NULL},
	        {TIFF_TAG_EXPOSURE_TIME, TIFF_TYPE_RATIONAL, 1, 0, exposureTime},
	        {TIFF_TAG_ISO_SPEED_RATINGS, TIFF_TYPE_SHORT, 1, iso, NULL},
	};
	bool exposureUnknown = !meta || meta->exposureSeconds <= 0;
	int count = drop_empty_entries(entries, ARRAY_COUNT(entries), exposureUnknown, baseline[0] != 0);
	return write_dng(path, entries, count, samples, (u32)width * height);
}

bool dng_file_save_raw(const char *path, const u8 *raw, int width, int height, int flip,
                       const float *whiteBalance, const float *blackLevels, const PhotoMeta *meta,
                       const float *calibration)
{
	size_t pixelCount = (size_t)width * height;
	u16 *samples = malloc(pixelCount * BYTES_PER_SAMPLE);
	if (!samples) {
		return false;
	}
	for (size_t i = 0; i < pixelCount; i++) {
		samples[i] = (u16)raw10_unpack(raw + i * RAW10_BYTES_PER_PIXEL);
	}
	bool saved = dng_file_save_mosaic(path, samples, width, height, RAW10_WHITE, flip, whiteBalance,
	                                  blackLevels, meta, calibration, 0);
	free(samples);
	return saved;
}

static u32 read_u16(const u8 *file, u32 offset)
{
	return (u32)file[offset] | (u32)file[offset + 1] << 8;
}

static u32 read_u32(const u8 *file, u32 offset)
{
	return read_u16(file, offset) | read_u16(file, offset + 2) << 16;
}

static float read_rational(const u8 *file, u32 offset)
{
	return (float)read_u32(file, offset) / read_u32(file, offset + sizeof(u32));
}

static float read_signed_rational(const u8 *file, u32 offset)
{
	return (float)(s32)read_u32(file, offset) / (s32)read_u32(file, offset + sizeof(u32));
}

// True when size bytes from offset lie inside the file. No sum here can wrap, whatever offset a file holds.
static bool in_file(size_t length, u32 offset, size_t size)
{
	return offset <= length && size <= length - offset;
}

// A single rational inside the file with a denominator that is not 0.
static bool rational_readable(const u8 *file, size_t length, u32 offset)
{
	return in_file(length, offset, TIFF_RATIONAL_SIZE) && read_u32(file, offset + sizeof(u32));
}

static void read_black_levels(const u8 *file, u32 offset, DngInfo *info)
{
	for (int cell = 0; cell < CFA_CELLS; cell++) {
		info->blackLevels[cell] = (float)read_u32(file, offset + cell * sizeof(u32));
	}
}

static void read_neutral(const u8 *file, u32 offset, DngInfo *info)
{
	for (int channel = 0; channel < RGB_CHANNELS; channel++) {
		u32 rational = offset + channel * TIFF_RATIONAL_SIZE;
		u32 numerator = read_u32(file, rational);
		u32 denominator = read_u32(file, rational + sizeof(u32));
		if (numerator && denominator) {
			info->neutral[channel] = (float)numerator / denominator;
		}
	}
}

static void read_entry(const u8 *file, size_t length, u32 entryOffset, DngInfo *info, DngImageSize *size)
{
	u32 tag = read_u16(file, entryOffset);
	u32 type = read_u16(file, entryOffset + TIFF_ENTRY_TYPE_OFFSET);
	u32 count = read_u32(file, entryOffset + TIFF_ENTRY_COUNT_OFFSET);
	u32 valueOffset = entryOffset + TIFF_ENTRY_VALUE_OFFSET;
	bool oneShort = type == TIFF_TYPE_SHORT && count == 1;
	u32 value = oneShort ? read_u16(file, valueOffset) : read_u32(file, valueOffset);
	bool rationalType = type == TIFF_TYPE_RATIONAL || type == TIFF_TYPE_SRATIONAL;
	switch (tag) {
	case TIFF_TAG_IMAGE_WIDTH:
		size->width = value;
		break;
	case TIFF_TAG_IMAGE_LENGTH:
		size->height = value;
		break;
	case TIFF_TAG_BITS_PER_SAMPLE:
		size->bitsPerSample = value;
		break;
	case TIFF_TAG_STRIP_OFFSETS:
		info->stripOffset = value;
		break;
	case TIFF_TAG_ORIENTATION:
		info->orientation = (int)value;
		break;
	case TIFF_TAG_ISO_SPEED_RATINGS:
		info->iso = (int)value;
		break;
	case DNG_TAG_WHITE_LEVEL:
		if (count == 1 && value > 0) {
			info->whiteLevel = value;
		}
		break;
	case DNG_TAG_BLACK_LEVEL:
		if (count == CFA_CELLS && type == TIFF_TYPE_LONG && in_file(length, value, CFA_CELLS * sizeof(u32))) {
			read_black_levels(file, value, info);
		}
		break;
	case DNG_TAG_AS_SHOT_NEUTRAL:
		if (count == RGB_CHANNELS && type == TIFF_TYPE_RATIONAL &&
		    in_file(length, value, RGB_CHANNELS * TIFF_RATIONAL_SIZE)) {
			read_neutral(file, value, info);
		}
		break;
	case TIFF_TAG_EXPOSURE_TIME:
		if (count == 1 && type == TIFF_TYPE_RATIONAL && rational_readable(file, length, value)) {
			info->exposureSeconds = read_rational(file, value);
		}
		break;
	case DNG_TAG_BASELINE_EXPOSURE:
		if (count == 1 && rationalType && rational_readable(file, length, value)) {
			bool isSigned = type == TIFF_TYPE_SRATIONAL;
			info->baselineEv = isSigned ? read_signed_rational(file, value) : read_rational(file, value);
		}
		break;
	}
}

// Reads Pro Cam's own DNGs: little-endian TIFF, one IFD, one uncompressed strip of 16-bit samples.
// Other files are refused.
static bool dng_info(const u8 *file, size_t length, DngInfo *info)
{
	memset(info, 0, sizeof *info);
	info->orientation = ORIENTATION_NORMAL;
	// A DNG without WhiteLevel holds 10-bit samples. Without a usable BlackLevel (4 x LONG), their black is
	// the sensor's, which the writer saves for a raw frame.
	info->whiteLevel = RAW10_WHITE;
	for (int cell = 0; cell < CFA_CELLS; cell++) {
		info->blackLevels[cell] = RAW10_BLACK;
	}
	for (int channel = 0; channel < RGB_CHANNELS; channel++) {
		info->neutral[channel] = 1;
	}
	if (length < MIN_FILE_SIZE || file[0] != 'I' || file[1] != 'I') { // little-endian TIFF only
		return false;
	}
	u32 ifdOffset = read_u32(file, TIFF_FIRST_IFD_POINTER);
	if (!in_file(length, ifdOffset, TIFF_IFD_COUNT_SIZE)) {
		return false;
	}
	DngImageSize size = {0, 0, 0};
	u32 entriesOffset = ifdOffset + TIFF_IFD_COUNT_SIZE;
	size_t entryCount = read_u16(file, ifdOffset);
	// Entries that run past the end of the file are not read.
	size_t entriesInFile = (length - entriesOffset) / TIFF_IFD_ENTRY_SIZE;
	if (entryCount > entriesInFile) {
		entryCount = entriesInFile;
	}
	for (size_t i = 0; i < entryCount; i++) {
		read_entry(file, length, entriesOffset + (u32)(i * TIFF_IFD_ENTRY_SIZE), info, &size);
	}
	bool sizeValid = size.width >= 1 && size.height >= 1 && size.width <= MAX_IMAGE_SIZE &&
	                 size.height <= MAX_IMAGE_SIZE;
	if (!sizeValid || size.bitsPerSample != DNG_BITS_PER_SAMPLE) {
		return false;
	}
	info->width = (int)size.width;
	info->height = (int)size.height;
	size_t sampleBytes = (size_t)size.width * size.height * BYTES_PER_SAMPLE;
	return info->stripOffset > 0 && in_file(length, info->stripOffset, sampleBytes);
}

// Samples above the white level are clipped to it.
static void read_samples(const u8 *strip, size_t sampleCount, u32 whiteLevel, u16 *samples)
{
	for (size_t i = 0; i < sampleCount; i++) {
		u32 sample = read_u16(strip, i * BYTES_PER_SAMPLE);
		samples[i] = (u16)(sample > whiteLevel ? whiteLevel : sample);
	}
}

u8 *dng_file_decode_rgb(const u8 *file, size_t length, int *outWidth, int *outHeight)
{
	DngInfo info;
	if (!dng_info(file, length, &info)) {
		return NULL;
	}
	bool fitsView = info.width <= RAW_VIEW_MAX_WIDTH && info.height <= CAM_FRAME_HEIGHT;
	bool whiteUsable = info.whiteLevel >= MIN_WHITE_LEVEL && info.whiteLevel <= UINT16_MAX;
	if (!fitsView || !whiteUsable) {
		return NULL;
	}
	int width = info.width;
	int height = info.height;
	size_t pixelCount = (size_t)width * height;
	u16 *samples = malloc(pixelCount * BYTES_PER_SAMPLE);
	u8 *rgb = malloc(pixelCount * RGB_CHANNELS);
	if (!samples || !rgb) {
		free(samples);
		free(rgb);
		return NULL;
	}
	read_samples(file + info.stripOffset, pixelCount, info.whiteLevel, samples);
	float whiteBalance[RGB_CHANNELS] = {info.neutral[1] / info.neutral[0], 1,
	                                    info.neutral[1] / info.neutral[2]};
	float blackSum = info.blackLevels[0] + info.blackLevels[1] + info.blackLevels[2] + info.blackLevels[3];
	int black = (int)(blackSum / CFA_CELLS + 0.5f);
	if (black >= (int)info.whiteLevel) {
		black = 0; // nonsense levels: show the picture anyway
	}
	// An HDR merge's white lies baselineEv above the metered exposure's white. Like its JPG, it is shown at
	// that exposure, with the extra range rolled off.
	bool hasBaseline = info.baselineEv > MIN_BASELINE_EV && info.baselineEv < MAX_BASELINE_EV;
	float range = hasBaseline ? exp2f(info.baselineEv) : 1;
	bool rendered = look_render_hdr(samples, width, height, black, (int)info.whiteLevel, range, whiteBalance,
	                                LOOK_INDEX_STANDARD, rgb);
	free(samples);
	if (!rendered) {
		free(rgb);
		return NULL;
	}
	bool knownOrientation = info.orientation >= 0 && info.orientation < ORIENTATION_COUNT;
	int flip = FLIP_OF_ORIENTATION[knownOrientation ? info.orientation : 0];
	if (flip) {
		image_files_flip_rgb(rgb, width, height, flip);
	}
	*outWidth = width;
	*outHeight = height;
	return rgb;
}
