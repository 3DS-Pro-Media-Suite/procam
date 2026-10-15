// PHOTO and BRACKET shots: one frame per photo or an exposure series, queued for saving.
#pragma once
#include <3ds.h>
#include "settings.h"

// Take one photo, or with `series` the BRACKET series around the metered exposure in one folder.
void still_shot_take(const ShotSettings *shot, bool series);
