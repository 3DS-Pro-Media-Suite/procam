// Key repeat for held buttons and sticks.
#pragma once
#include <3ds.h>

// True when the last firing came from a held key's repeat, not from the press.
extern bool g_keyRepeated;

// Fires on the first active frame, then every `repeatEvery` frames after a short hold. `heldFrames` is the
// caller's counter for this key.
bool key_repeat_fires(bool active, int *heldFrames, int repeatEvery);

// The same for one D-pad direction (KEY_DUP, KEY_DDOWN, KEY_DLEFT or KEY_DRIGHT). The shooting screen, the
// setup menu and the value picker share these counters, so a key held across a change between them keeps its
// repeat. The look editor keeps its own.
bool key_repeat_dpad(u32 held, u32 dpadKey, int repeatEvery);
