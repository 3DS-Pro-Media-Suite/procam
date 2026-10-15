#include "merge_defects.h"
#include <stdbool.h>
#include <stdlib.h>
#include "merge_common.h"
#include "raw10.h"

#define DEFECT_ENTRY_VALUES 2 // a defect map entry: x, then y

// A defect becomes the median of its same-colour neighbours, which are at most 2 pixels away.
#define DEFECT_NEIGHBOUR_REACH 2
#define GREEN_NEIGHBOURS 8
#define RED_BLUE_NEIGHBOURS 4

// (y, x) of a pixel's same-colour neighbours: red and blue use the first four, green all eight.
static const int DEFECT_NEIGHBOURS[GREEN_NEIGHBOURS][AXIS_COUNT] = {{0, -2},  {0, 2},  {-2, 0}, {2, 0},
                                                                    {-1, -1}, {-1, 1}, {1, -1}, {1, 1}};

void merge_defects_unpack(uint8_t *frame)
{
	uint16_t *pixels = (uint16_t *)(void *)frame;
	for (int i = 0; i < MERGE_WIDTH * MERGE_HEIGHT; i++) {
		pixels[i] = (uint16_t)raw10_unpack(frame + i * RAW10_BYTES_PER_PIXEL);
	}
}

static uint16_t neighbour_median(const uint16_t *frame, int x, int y)
{
	bool isGreen = (x ^ y) & 1;
	int count = isGreen ? GREEN_NEIGHBOURS : RED_BLUE_NEIGHBOURS;
	int values[GREEN_NEIGHBOURS];
	for (int i = 0; i < count; i++) {
		int neighbourY = y + DEFECT_NEIGHBOURS[i][AXIS_Y];
		values[i] = frame[neighbourY * MERGE_WIDTH + x + DEFECT_NEIGHBOURS[i][AXIS_X]];
	}
	for (int i = 1; i < count; i++) {
		for (int j = i; j > 0 && values[j - 1] > values[j]; j--) {
			int swap = values[j];
			values[j] = values[j - 1];
			values[j - 1] = swap;
		}
	}
	int middle = count / 2;
	return (uint16_t)((values[middle - 1] + values[middle]) / 2);
}

int merge_defects_fix(uint16_t *frame, const uint16_t *defects, int defectCount)
{
	if (!defects || defectCount <= 0) {
		return 0;
	}
	int *positions = malloc(sizeof(int) * defectCount);
	uint16_t *medians = malloc(sizeof(uint16_t) * defectCount);
	if (!positions || !medians) {
		free(positions);
		free(medians);
		return 0;
	}
	int fixCount = 0;
	for (int i = 0; i < defectCount; i++) {
		const uint16_t *entry = &defects[i * DEFECT_ENTRY_VALUES];
		int x = entry[0];
		int y = entry[1];
		if (x < DEFECT_NEIGHBOUR_REACH || x >= MERGE_WIDTH - DEFECT_NEIGHBOUR_REACH ||
		    y < DEFECT_NEIGHBOUR_REACH || y >= MERGE_HEIGHT - DEFECT_NEIGHBOUR_REACH) {
			continue; // too close to the edge for all neighbours
		}
		uint16_t median = neighbour_median(frame, x, y);
		positions[fixCount] = y * MERGE_WIDTH + x;
		medians[fixCount] = median;
		fixCount++;
	}
	int changed = 0;
	for (int i = 0; i < fixCount; i++) {
		if (frame[positions[i]] != medians[i]) {
			frame[positions[i]] = medians[i];
			changed++;
		}
	}
	free(positions);
	free(medians);
	return changed;
}
