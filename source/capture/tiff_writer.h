// Little-endian TIFF pieces shared by the EXIF block and the DNG writer.
#pragma once
#include <3ds.h>
#include <time.h>

#define TIFF_FIRST_IFD_POINTER 4 // where the header stores the offset of the first IFD
#define TIFF_HEADER_SIZE 8
#define TIFF_IFD_COUNT_SIZE 2
#define TIFF_IFD_ENTRY_SIZE 12
#define TIFF_NEXT_IFD_SIZE 4
#define TIFF_ENTRY_TYPE_OFFSET 2
#define TIFF_ENTRY_COUNT_OFFSET 4
#define TIFF_ENTRY_VALUE_OFFSET 8
#define TIFF_INLINE_VALUE_SIZE 4 // larger values are stored outside the entry
#define TIFF_VALUE_ALIGNMENT 2 // values outside an entry start on a word boundary
#define TIFF_RATIONAL_SIZE 8
#define TIFF_DATE_TIME_SIZE 20 // "YYYY:MM:DD HH:MM:SS" and the terminating zero

enum {
	TIFF_TYPE_BYTE = 1,
	TIFF_TYPE_ASCII = 2,
	TIFF_TYPE_SHORT = 3,
	TIFF_TYPE_LONG = 4,
	TIFF_TYPE_RATIONAL = 5,
	TIFF_TYPE_SLONG = 9,
	TIFF_TYPE_SRATIONAL = 10,
};

enum {
	TIFF_TAG_MAKE = 271,
	TIFF_TAG_MODEL = 272,
	TIFF_TAG_SOFTWARE = 305,
	TIFF_TAG_DATE_TIME = 306,
	TIFF_TAG_EXPOSURE_TIME = 33434,
	TIFF_TAG_ISO_SPEED_RATINGS = 34855,
};

void tiff_writer_put_u16(u8 *out, u16 value);
void tiff_writer_put_u32(u8 *out, u32 value);
// The TIFF header; the first IFD follows it directly.
void tiff_writer_put_header(u8 *out);
// An IFD entry's tag, type and count. Its value goes at TIFF_ENTRY_VALUE_OFFSET.
void tiff_writer_put_entry(u8 *entry, u16 tag, u16 type, u32 count);
// A time as TIFF DateTime text. Without a usable time (when NULL), a fixed placeholder date.
void tiff_writer_date_time(const time_t *when, char out[TIFF_DATE_TIME_SIZE]);
// An exposure time as a TIFF RATIONAL (numerator, denominator). The numerator is 0 when it is unknown.
void tiff_writer_exposure_rational(float seconds, u32 rational[2]);
