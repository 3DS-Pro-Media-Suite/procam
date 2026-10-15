#include "white_balance.h"
#include <3ds.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "math_util.h"

#define DARK_CHANNEL_SUM 64 // grid sums up to this are too dark to balance
#define MIN_GREY_GREEN 8 // darker cells are mostly noise
#define MIN_NEED_SAMPLE 2 // a cell's red and blue need at least this for a usable gain
// Bands around grey, as balanced red or blue over green.
#define NEAR_GREY_LOW 0.8187f // exp(-0.2)
#define NEAR_GREY_HIGH 1.2214f // exp(0.2)
#define WIDE_GREY_LOW 0.7408f // exp(-0.3)
#define WIDE_GREY_HIGH 1.3499f // exp(0.3)
#define GREY_PASSES 4
#define MIN_GREY_CELLS 20
#define NEED_PERCENTILE_LOW 60
#define NEED_PERCENTILE_HIGH 80
#define MIN_GAIN 0.4f
#define MAX_GAIN 4.0f
#define GAIN_EASING 0.2f // each frame moves the gains this part of the way to the estimate
#define GAIN_STEPS 64 // 1/64 steps, so the live view's table is not rebuilt for every tiny change

// The gains a near-grey cell needs, as logarithms.
typedef struct {
	float sortKey; // red and blue together: how far towards magenta the cell pulls
	float logRedGain;
	float logBlueGain;
} GainNeed;

typedef struct {
	float red;
	float blue;
} Gains;

static float g_gains[RGB_CHANNELS] = {1.0f, 1.0f, 1.0f};
static GainNeed g_gainNeeds[RAW_GRID_MAX_CELLS];

static bool outside_band(float value, float reference, float low, float high)
{
	return value < reference * low || value > reference * high;
}

// Grey world misreads scenes dominated by one colour. So the estimate is refined a few times over on the
// cells that look nearly grey under it. Brighter cells weigh more.
static Gains grey_cell_gains(const RawGrid *grid, Gains gains)
{
	for (int pass = 0; pass < GREY_PASSES; pass++) {
		float redWeighted = 0;
		float greenWeighted = 0;
		float blueWeighted = 0;
		int used = 0;
		for (int i = 0; i < grid->storedCells; i++) {
			float green = grid->green[i];
			float red = grid->red[i] * gains.red;
			float blue = grid->blue[i] * gains.blue;
			if (grid->clipped[i] || green < MIN_GREY_GREEN ||
			    outside_band(red, green, NEAR_GREY_LOW, NEAR_GREY_HIGH) ||
			    outside_band(blue, green, NEAR_GREY_LOW, NEAR_GREY_HIGH)) {
				continue;
			}
			redWeighted += grid->red[i] * green;
			greenWeighted += green * green;
			blueWeighted += grid->blue[i] * green;
			used++;
		}
		// Too few grey cells: keep the estimate.
		if (used < MIN_GREY_CELLS || redWeighted <= 0 || blueWeighted <= 0) {
			break;
		}
		gains.red = greenWeighted / redWeighted;
		gains.blue = greenWeighted / blueWeighted;
	}
	return gains;
}

static int compare_gain_need(const void *a, const void *b)
{
	float keyA = ((const GainNeed *)a)->sortKey;
	float keyB = ((const GainNeed *)b)->sortKey;
	if (keyA < keyB) {
		return -1;
	}
	return keyA > keyB;
}

// Near-infrared pulls sunlit neutrals towards 1:1:1. So, of the cells near grey, the gains are those the 60th
// to 80th percentile need, counted towards magenta. The sensor's own white balance also errs magenta, never
// green.
static Gains lean_magenta(const RawGrid *grid, Gains gains)
{
	int count = 0;
	for (int i = 0; i < grid->storedCells; i++) {
		float green = grid->green[i];
		float red = grid->red[i] * gains.red;
		float blue = grid->blue[i] * gains.blue;
		if (grid->clipped[i] || green < MIN_GREY_GREEN || grid->red[i] < MIN_NEED_SAMPLE ||
		    grid->blue[i] < MIN_NEED_SAMPLE || outside_band(red, green, WIDE_GREY_LOW, WIDE_GREY_HIGH) ||
		    outside_band(blue, green, WIDE_GREY_LOW, WIDE_GREY_HIGH)) {
			continue;
		}
		GainNeed *need = &g_gainNeeds[count];
		need->logRedGain = logf(green / grid->red[i]);
		need->logBlueGain = logf(green / grid->blue[i]);
		need->sortKey = need->logRedGain + need->logBlueGain;
		count++;
	}
	if (count < MIN_GREY_CELLS) {
		return gains;
	}
	qsort(g_gainNeeds, count, sizeof g_gainNeeds[0], compare_gain_need);
	int first = count * NEED_PERCENTILE_LOW / 100;
	int end = count * NEED_PERCENTILE_HIGH / 100;
	if (end <= first) {
		end = first + 1;
	}
	float logRedSum = 0;
	float logBlueSum = 0;
	for (int i = first; i < end; i++) {
		logRedSum += g_gainNeeds[i].logRedGain;
		logBlueSum += g_gainNeeds[i].logBlueGain;
	}
	gains.red = expf(logRedSum / (end - first));
	gains.blue = expf(logBlueSum / (end - first));
	return gains;
}

// The viewing gains ease towards the estimate and move in whole steps.
static void ease_gains(Gains target)
{
	g_gains[RGB_RED] += (target.red - g_gains[RGB_RED]) * GAIN_EASING;
	g_gains[RGB_BLUE] += (target.blue - g_gains[RGB_BLUE]) * GAIN_EASING;
	g_gains[RGB_RED] = roundf(g_gains[RGB_RED] * GAIN_STEPS) / GAIN_STEPS;
	g_gains[RGB_BLUE] = roundf(g_gains[RGB_BLUE] * GAIN_STEPS) / GAIN_STEPS;
}

void white_balance_update(const RawGrid *grid)
{
	if (grid->redSum <= DARK_CHANNEL_SUM || grid->blueSum <= DARK_CHANNEL_SUM) {
		return;
	}
	Gains greyWorld;
	greyWorld.red = (float)grid->greenSum / grid->redSum;
	greyWorld.blue = (float)grid->greenSum / grid->blueSum;
	Gains gains = grey_cell_gains(grid, greyWorld);
	gains = lean_magenta(grid, gains);
	gains.red = math_clamp_float(gains.red, MIN_GAIN, MAX_GAIN);
	gains.blue = math_clamp_float(gains.blue, MIN_GAIN, MAX_GAIN);
	ease_gains(gains);
}

void white_balance_get(float gains[RGB_CHANNELS])
{
	memcpy(gains, g_gains, sizeof g_gains);
}
