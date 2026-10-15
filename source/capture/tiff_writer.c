#include "tiff_writer.h"
#include <3ds.h>
#include <string.h>
#include <time.h>

#define TIFF_BYTE_ORDER_AND_MAGIC "II*" // little-endian byte order, then the number 42 with its zero byte
#define TIFF_PLACEHOLDER_DATE_TIME "2000:01:01 00:00:00"
#define TIFF_DATE_TIME_FORMAT "%Y:%m:%d %H:%M:%S"

// Times below the limit are stored in 1/100000000 s, so 1/32000 s stays exact. Up to the limit the numerator
// fits in 32 bits; longer times are stored in milliseconds.
#define FINE_EXPOSURE_LIMIT_SECONDS 40
#define FINE_EXPOSURE_UNITS_PER_SECOND 100000000
#define COARSE_EXPOSURE_UNITS_PER_SECOND 1000

void tiff_writer_put_u16(u8 *out, u16 value)
{
	out[0] = value;
	out[1] = value >> 8;
}

void tiff_writer_put_u32(u8 *out, u32 value)
{
	out[0] = value;
	out[1] = value >> 8;
	out[2] = value >> 16;
	out[3] = value >> 24;
}

void tiff_writer_put_header(u8 *out)
{
	memcpy(out, TIFF_BYTE_ORDER_AND_MAGIC, TIFF_FIRST_IFD_POINTER);
	tiff_writer_put_u32(out + TIFF_FIRST_IFD_POINTER, TIFF_HEADER_SIZE);
}

void tiff_writer_put_entry(u8 *entry, u16 tag, u16 type, u32 count)
{
	tiff_writer_put_u16(entry, tag);
	tiff_writer_put_u16(entry + TIFF_ENTRY_TYPE_OFFSET, type);
	tiff_writer_put_u32(entry + TIFF_ENTRY_COUNT_OFFSET, count);
}

void tiff_writer_date_time(const time_t *when, char out[TIFF_DATE_TIME_SIZE])
{
	memcpy(out, TIFF_PLACEHOLDER_DATE_TIME, TIFF_DATE_TIME_SIZE);
	struct tm fields;
	if (when && gmtime_r(when, &fields)) {
		strftime(out, TIFF_DATE_TIME_SIZE, TIFF_DATE_TIME_FORMAT, &fields);
	}
}

void tiff_writer_exposure_rational(float seconds, u32 rational[2])
{
	if (seconds <= 0) {
		rational[0] = 0;
		rational[1] = FINE_EXPOSURE_UNITS_PER_SECOND;
	} else if (seconds < FINE_EXPOSURE_LIMIT_SECONDS) {
		rational[0] = (u32)(seconds * FINE_EXPOSURE_UNITS_PER_SECOND + 0.5f);
		rational[1] = FINE_EXPOSURE_UNITS_PER_SECOND;
	} else {
		rational[0] = (u32)(seconds * COARSE_EXPOSURE_UNITS_PER_SECOND + 0.5f);
		rational[1] = COARSE_EXPOSURE_UNITS_PER_SECOND;
	}
}
