#include "raw_grid.h"
#include <3ds.h>
#include "raw_view.h"

#define CLIPPED_SAMPLE 1000 // a cell with a sample above this is clipped

void raw_grid_sample(RawGrid *grid, const u8 *frame, int width, int height)
{
	grid->redSum = 0;
	grid->greenSum = 0;
	grid->blueSum = 0;
	grid->cells = 0;
	for (int y = 0; y + 1 < height; y += RAW_GRID_SPACING) {
		for (int x = 0; x + 1 < width; x += RAW_GRID_SPACING) {
			int red = raw_view_sample(frame, width, y, x);
			int green = raw_view_sample(frame, width, y, x + 1);
			int blue = raw_view_sample(frame, width, y + 1, x + 1);
			grid->redSum += raw_view_above_black(red);
			grid->greenSum += raw_view_above_black(green);
			grid->blueSum += raw_view_above_black(blue);
			if (grid->cells < RAW_GRID_MAX_CELLS) {
				grid->red[grid->cells] = raw_view_above_black(red);
				grid->green[grid->cells] = raw_view_above_black(green);
				grid->blue[grid->cells] = raw_view_above_black(blue);
				grid->clipped[grid->cells] =
				        red > CLIPPED_SAMPLE || green > CLIPPED_SAMPLE || blue > CLIPPED_SAMPLE;
			}
			grid->cells++;
		}
	}
	grid->storedCells = grid->cells < RAW_GRID_MAX_CELLS ? (int)grid->cells : RAW_GRID_MAX_CELLS;
}
