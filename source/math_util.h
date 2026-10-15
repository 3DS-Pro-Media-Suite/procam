// Clamps and array sizes shared by the app and its libraries.
#pragma once

#define ARRAY_COUNT(array) ((int)(sizeof(array) / sizeof((array)[0])))

static inline int math_clamp_int(int value, int low, int high)
{
	if (value < low) {
		return low;
	}
	if (value > high) {
		return high;
	}
	return value;
}

static inline float math_clamp_float(float value, float low, float high)
{
	if (value < low) {
		return low;
	}
	if (value > high) {
		return high;
	}
	return value;
}
