// A sparse grid of RGGB cells from a live raw frame, black subtracted: what the white balance and the meter
// look at.
#pragma once
#include <3ds.h>

#define RAW_GRID_SPACING 16 // pixels between the grid's cells, across and down
#define RAW_GRID_MAX_CELLS 1600 // more than the 40 x 30 cells of a full frame

typedef struct {
	u16 red[RAW_GRID_MAX_CELLS];
	u16 green[RAW_GRID_MAX_CELLS];
	u16 blue[RAW_GRID_MAX_CELLS];
	u8 clipped[RAW_GRID_MAX_CELLS]; // a sample of the cell is near white
	// Over every cell of the frame, also the ones past the arrays.
	u32 redSum;
	u32 greenSum;
	u32 blueSum;
	u32 cells;
	int storedCells; // the cells in the arrays
} RawGrid;

void raw_grid_sample(RawGrid *grid, const u8 *frame, int width, int height);
