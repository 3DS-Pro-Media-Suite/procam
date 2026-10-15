#include "hot_pixel_learner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "math_util.h"
#include "raw10.h"

// The map and the counts use full-frame coordinates.
#define FULL_FRAME_WIDTH 640
#define FULL_FRAME_HEIGHT 480
#define FULL_FRAME_PIXELS (FULL_FRAME_WIDTH * FULL_FRAME_HEIGHT)

// The fast mode reads row pairs 0-1, 4-5, 8-9 and so on. Its frame starts 2 rows into that readout, while
// the full frame starts 4 rows into the sensor.
#define FAST_ROW_PAIR_PITCH 4
#define FAST_ROWS_PER_PAIR 2
#define FAST_FRAME_FIRST_ROW 2
#define FULL_FRAME_FIRST_ROW 4

// In an RGGB mosaic the same-colour neighbours sit two pixels away.
#define SAME_COLOUR_STEP 2
#define NEIGHBOUR_COUNT 8

// Each feed checks one slice of the rows, so a full pass takes SLICE_COUNT feeds.
#define SLICE_COUNT 8
#define FRAMES_PER_MAP_REBUILD 4
#define MIN_FRAME_SIZE 16

// The scene check compares one green sample every SCENE_GRID_SPACING rows and columns with the last checked
// frame's. Too little change would count the same detail again.
#define SCENE_GRID_SPACING 16
#define SCENE_CHANGE_SCALE 4 // 10-bit differences to the 0..255 scale of MIN_SCENE_CHANGE
#define MIN_SCENE_CHANGE 6

// Hot pixels are only judged in dark areas, dead ones only in lit, unclipped areas. The first two levels are
// above black, the clip level is raw.
#define HOT_CHECK_MAX_LEVEL 400
#define DEAD_CHECK_MIN_LEVEL 60
#define CLIPPED_LEVEL 1000
// A pixel stands out when it differs from its neighbours by this margin plus a quarter of their level.
#define STAND_OUT_MARGIN 20
#define STAND_OUT_LEVEL_DIVISOR 4

// A pixel is stuck when at least 7 in 10 of at least MIN_CHECKS checks flagged it.
#define MIN_CHECKS 48
#define STUCK_HITS 7
#define STUCK_OUT_OF 10

// The counts file: a header, then per kept pixel its u32 index and its four counts.
#define COUNTS_FILE_MAGIC 0x4E524C48u // "HLRN" as a little-endian u32
#define COUNTS_FILE_VERSION 2
#define COUNTS_FILE_PATH "%s/hot_cam%d.bin"
#define COUNTS_PATH_SIZE 160
// The records are small, so a large buffer keeps the card writes few.
#define COUNTS_FILE_BUFFER_SIZE (64 * 1024)
// Pixels flagged fewer times are not saved. They restart from zero, which only delays them.
#define MIN_HITS_TO_SAVE 2

// The checks of one full-frame pixel. Saved to the card as they are.
typedef struct {
	uint8_t hotChecks;
	uint8_t hotHits;
	uint8_t deadChecks;
	uint8_t deadHits;
} PixelCounts;

typedef struct {
	uint32_t magic;
	uint32_t version;
	uint32_t framesChecked;
	uint32_t pixelCount;
} CountsFileHeader;

// A built map: count pixels as x, y pairs.
typedef struct {
	int count;
	uint16_t xy[HOT_PIXEL_MAP_MAX * 2];
} HotPixelMap;

typedef struct {
	PixelCounts *counts; // one per full-frame pixel
	unsigned framesChecked; // full passes over the rows
	unsigned slicesChecked;
	int nextSlice;
	// Double-buffered: readers use the published map while the other one is built. A map is published once
	// complete, so a reader on another core sees all of it with its own count.
	HotPixelMap maps[2];
	_Atomic(const HotPixelMap *) published; // NULL until the first build
	bool unsaved;
} CameraLearner;

static CameraLearner g_learners[HOT_PIXEL_CAMERAS];

static inline int raw_value(const uint8_t *raw, int width, int y, int x)
{
	return raw10_unpack(raw + ((size_t)y * width + x) * RAW10_BYTES_PER_PIXEL);
}

static bool allocate_counts(CameraLearner *learner)
{
	if (!learner->counts) {
		learner->counts = calloc((size_t)FULL_FRAME_PIXELS, sizeof(PixelCounts));
	}
	return learner->counts != NULL;
}

// Returns the full-frame row of a frame row. Negative: the row is not in the full frame.
static inline int full_frame_row(int row, bool fastMode)
{
	if (!fastMode) {
		return row;
	}
	int readoutRow = row + FAST_FRAME_FIRST_ROW;
	int pairIndex = readoutRow >> 1;
	int rowInPair = readoutRow & 1;
	return FAST_ROW_PAIR_PITCH * pairIndex + rowInPair - FULL_FRAME_FIRST_ROW;
}

static bool flagged_often(uint8_t checks, uint8_t hits)
{
	return checks >= MIN_CHECKS && hits * STUCK_OUT_OF >= checks * STUCK_HITS;
}

static bool is_stuck(const PixelCounts *counts)
{
	return flagged_often(counts->hotChecks, counts->hotHits) ||
	       flagged_often(counts->deadChecks, counts->deadHits);
}

static void rebuild_map(CameraLearner *learner)
{
	HotPixelMap *map = learner->published == &learner->maps[0] ? &learner->maps[1] : &learner->maps[0];
	int count = 0;
	for (int i = 0; i < FULL_FRAME_PIXELS && count < HOT_PIXEL_MAP_MAX; i++) {
		if (!is_stuck(&learner->counts[i])) {
			continue;
		}
		map->xy[count * 2] = (uint16_t)(i % FULL_FRAME_WIDTH);
		map->xy[count * 2 + 1] = (uint16_t)(i / FULL_FRAME_WIDTH);
		count++;
	}
	map->count = count;
	learner->published = map;
}

// Both counts are halved before the check count overflows, which keeps their ratio.
static void count_check(uint8_t *checks, uint8_t *hits, bool hit)
{
	if (*checks == UINT8_MAX) {
		*checks /= 2;
		*hits /= 2;
	}
	(*checks)++;
	if (hit) {
		(*hits)++;
	}
}

static int stand_out_margin(int level)
{
	return STAND_OUT_MARGIN + level / STAND_OUT_LEVEL_DIVISOR;
}

static void check_pixel(PixelCounts *counts, const uint8_t *raw, int width, int row, int x, int blackLevel)
{
	int above = row - SAME_COLOUR_STEP;
	int below = row + SAME_COLOUR_STEP;
	int left = x - SAME_COLOUR_STEP;
	int right = x + SAME_COLOUR_STEP;
	int neighbours[NEIGHBOUR_COUNT] = {
	        raw_value(raw, width, above, left),  raw_value(raw, width, above, x),
	        raw_value(raw, width, above, right), raw_value(raw, width, row, left),
	        raw_value(raw, width, row, right),   raw_value(raw, width, below, left),
	        raw_value(raw, width, below, x),     raw_value(raw, width, below, right),
	};
	int neighbourMax = neighbours[0];
	int neighbourMin = neighbours[0];
	for (int i = 1; i < NEIGHBOUR_COUNT; i++) {
		if (neighbours[i] > neighbourMax) {
			neighbourMax = neighbours[i];
		}
		if (neighbours[i] < neighbourMin) {
			neighbourMin = neighbours[i];
		}
	}
	int value = raw_value(raw, width, row, x) - blackLevel;
	int brightest = neighbourMax - blackLevel;
	int darkest = neighbourMin - blackLevel;
	if (brightest < HOT_CHECK_MAX_LEVEL) {
		bool hot = value - brightest > stand_out_margin(brightest);
		count_check(&counts->hotChecks, &counts->hotHits, hot);
	}
	if (darkest > DEAD_CHECK_MIN_LEVEL && brightest < CLIPPED_LEVEL - blackLevel) {
		bool dead = darkest - value > stand_out_margin(darkest);
		count_check(&counts->deadChecks, &counts->deadHits, dead);
	}
}

// One green sample of the scene grid, above black.
static uint16_t scene_sample(const uint8_t *raw, int width, int y, int x, int blackLevel)
{
	return (uint16_t)math_clamp_int(raw_value(raw, width, y, x + 1) - blackLevel, 0, RAW10_WHITE);
}

// The mean change of the scene grid's green since the feeder's last checked frame, 0..255. A feeder's first
// frame is all change.
static int scene_change(const HotPixelScene *scene, const uint8_t *raw, int width, int height, int blackLevel)
{
	if (!scene->have) {
		return UINT8_MAX;
	}
	uint32_t difference = 0;
	int count = 0;
	for (int y = 0; y + 1 < height; y += SCENE_GRID_SPACING) {
		for (int x = 0; x + 1 < width && count < HOT_PIXEL_SCENE_SAMPLES; x += SCENE_GRID_SPACING) {
			uint16_t sample = scene_sample(raw, width, y, x, blackLevel);
			uint16_t last = scene->green[count];
			difference += sample > last ? sample - last : last - sample;
			count++;
		}
	}
	return count ? (int)(difference / count / SCENE_CHANGE_SCALE) : 0;
}

static void remember_scene(HotPixelScene *scene, const uint8_t *raw, int width, int height, int blackLevel)
{
	int count = 0;
	for (int y = 0; y + 1 < height; y += SCENE_GRID_SPACING) {
		for (int x = 0; x + 1 < width && count < HOT_PIXEL_SCENE_SAMPLES; x += SCENE_GRID_SPACING) {
			scene->green[count] = scene_sample(raw, width, y, x, blackLevel);
			count++;
		}
	}
	scene->have = true;
}

// Checks one slice of the rows.
static void feed_slice(CameraLearner *learner, const uint8_t *raw, int width, int height, bool fastMode,
                       int blackLevel)
{
	int firstRow = learner->nextSlice * height / SLICE_COUNT;
	int endRow = (learner->nextSlice + 1) * height / SLICE_COUNT;
	learner->nextSlice = (learner->nextSlice + 1) & (SLICE_COUNT - 1);
	// The border rows and columns lack neighbours.
	if (firstRow < SAME_COLOUR_STEP) {
		firstRow = SAME_COLOUR_STEP;
	}
	if (endRow > height - SAME_COLOUR_STEP) {
		endRow = height - SAME_COLOUR_STEP;
	}
	for (int row = firstRow; row < endRow; row++) {
		int fullRow = full_frame_row(row, fastMode);
		if (fullRow < 0 || fullRow >= FULL_FRAME_HEIGHT) {
			continue;
		}
		PixelCounts *rowCounts = learner->counts + (size_t)fullRow * FULL_FRAME_WIDTH;
		for (int x = SAME_COLOUR_STEP; x < width - SAME_COLOUR_STEP; x++) {
			check_pixel(&rowCounts[x], raw, width, row, x, blackLevel);
		}
	}
	learner->unsaved = true;
	learner->slicesChecked++;
	if (learner->slicesChecked % SLICE_COUNT == 0) {
		learner->framesChecked++;
		if (learner->framesChecked % FRAMES_PER_MAP_REBUILD == 0) {
			rebuild_map(learner);
		}
	}
}

void hot_pixel_learner_feed_frame(int camera, HotPixelScene *scene, const uint8_t *raw, int width, int height,
                                  bool fastMode, int blackLevel, bool wholeFrame)
{
	if (camera < 0 || camera >= HOT_PIXEL_CAMERAS || !scene || !raw) {
		return;
	}
	if (width < MIN_FRAME_SIZE || height < MIN_FRAME_SIZE || width > FULL_FRAME_WIDTH) {
		return;
	}
	CameraLearner *learner = &g_learners[camera];
	if (scene_change(scene, raw, width, height, blackLevel) < MIN_SCENE_CHANGE) {
		return;
	}
	remember_scene(scene, raw, width, height, blackLevel);
	if (!allocate_counts(learner)) {
		return;
	}
	int slices = wholeFrame ? SLICE_COUNT : 1;
	for (int i = 0; i < slices; i++) {
		feed_slice(learner, raw, width, height, fastMode, blackLevel);
	}
}

int hot_pixel_learner_frame_row(int fullRow, bool fastMode)
{
	if (!fastMode) {
		return fullRow;
	}
	int rowInPitch = fullRow % FAST_ROW_PAIR_PITCH;
	if (rowInPitch >= FAST_ROWS_PER_PAIR) {
		return -1;
	}
	return fullRow / FAST_ROW_PAIR_PITCH * FAST_ROWS_PER_PAIR + rowInPitch;
}

int hot_pixel_learner_map(int camera, const uint16_t **xy)
{
	if (camera < 0 || camera >= HOT_PIXEL_CAMERAS) {
		*xy = NULL;
		return 0;
	}
	const HotPixelMap *map = g_learners[camera].published;
	*xy = map ? map->xy : NULL;
	return map ? map->count : 0;
}

static bool worth_saving(const PixelCounts *counts)
{
	return counts->hotHits >= MIN_HITS_TO_SAVE || counts->deadHits >= MIN_HITS_TO_SAVE;
}

void hot_pixel_learner_save(const char *dir)
{
	for (int camera = 0; camera < HOT_PIXEL_CAMERAS; camera++) {
		CameraLearner *learner = &g_learners[camera];
		if (!learner->unsaved || !learner->counts) {
			continue;
		}
		char path[COUNTS_PATH_SIZE];
		snprintf(path, sizeof path, COUNTS_FILE_PATH, dir, camera);
		FILE *file = fopen(path, "wb");
		if (!file) {
			continue;
		}
		setvbuf(file, NULL, _IOFBF, COUNTS_FILE_BUFFER_SIZE);
		CountsFileHeader header = {
		        .magic = COUNTS_FILE_MAGIC,
		        .version = COUNTS_FILE_VERSION,
		        .framesChecked = learner->framesChecked,
		        .pixelCount = 0,
		};
		for (int i = 0; i < FULL_FRAME_PIXELS; i++) {
			if (worth_saving(&learner->counts[i])) {
				header.pixelCount++;
			}
		}
		fwrite(&header, sizeof header, 1, file);
		for (int i = 0; i < FULL_FRAME_PIXELS; i++) {
			if (!worth_saving(&learner->counts[i])) {
				continue;
			}
			uint32_t index = (uint32_t)i;
			fwrite(&index, sizeof index, 1, file);
			fwrite(&learner->counts[i], sizeof(PixelCounts), 1, file);
		}
		fclose(file);
		learner->unsaved = false;
	}
}

void hot_pixel_learner_load(const char *dir)
{
	for (int camera = 0; camera < HOT_PIXEL_CAMERAS; camera++) {
		char path[COUNTS_PATH_SIZE];
		snprintf(path, sizeof path, COUNTS_FILE_PATH, dir, camera);
		FILE *file = fopen(path, "rb");
		if (!file) {
			continue;
		}
		setvbuf(file, NULL, _IOFBF, COUNTS_FILE_BUFFER_SIZE);
		CountsFileHeader header;
		CameraLearner *learner = &g_learners[camera];
		bool headerValid = fread(&header, sizeof header, 1, file) == 1 && header.magic == COUNTS_FILE_MAGIC &&
		                   header.version == COUNTS_FILE_VERSION;
		if (headerValid && allocate_counts(learner)) {
			learner->framesChecked = header.framesChecked;
			for (uint32_t record = 0; record < header.pixelCount; record++) {
				uint32_t index;
				PixelCounts counts;
				if (fread(&index, sizeof index, 1, file) != 1 ||
				    fread(&counts, sizeof counts, 1, file) != 1) {
					break;
				}
				if (index < (uint32_t)FULL_FRAME_PIXELS) {
					memcpy(&learner->counts[index], &counts, sizeof counts);
				}
			}
			rebuild_map(learner);
		}
		fclose(file);
	}
}
