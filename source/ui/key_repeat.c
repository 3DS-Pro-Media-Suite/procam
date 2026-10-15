#include "key_repeat.h"

// Frames a key is held before it starts to repeat.
#define REPEAT_DELAY_FRAMES 18

bool g_keyRepeated;

static int g_dpadUpFrames;
static int g_dpadDownFrames;
static int g_dpadLeftFrames;
static int g_dpadRightFrames;

bool key_repeat_fires(bool active, int *heldFrames, int repeatEvery)
{
	if (!active) {
		*heldFrames = 0;
		return false;
	}
	(*heldFrames)++;
	bool fire = *heldFrames == 1 || (*heldFrames > REPEAT_DELAY_FRAMES && *heldFrames % repeatEvery == 0);
	if (fire) {
		g_keyRepeated = *heldFrames > 1;
	}
	return fire;
}

static int *dpad_held_frames(u32 dpadKey)
{
	if (dpadKey == KEY_DUP) {
		return &g_dpadUpFrames;
	}
	if (dpadKey == KEY_DDOWN) {
		return &g_dpadDownFrames;
	}
	if (dpadKey == KEY_DLEFT) {
		return &g_dpadLeftFrames;
	}
	return &g_dpadRightFrames;
}

bool key_repeat_dpad(u32 held, u32 dpadKey, int repeatEvery)
{
	return key_repeat_fires((held & dpadKey) != 0, dpad_held_frames(dpadKey), repeatEvery);
}
