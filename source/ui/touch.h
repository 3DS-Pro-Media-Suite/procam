// Touch screen hit test.
#pragma once
#include <3ds.h>

static inline bool touch_in_rect(touchPosition touch, float x, float y, float width, float height)
{
	return touch.px >= x && touch.px < x + width && touch.py >= y && touch.py < y + height;
}
