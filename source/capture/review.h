// The image review mailbox and the shutter flash: the photo just taken, shown on the top screen.
#pragma once
#include <3ds.h>
#include "raw10.h"

#define REVIEW_SHOW_MS 1500 // how long a photo replaces the live view after the shot

bool review_init(void);
void review_exit(void);
void review_post_scaled(const u8 *pixels, int width, int height, int format, int showMs, float gain,
                        const float blackLevels[CFA_CELLS], const char *tag);
void review_post(const u8 *pixels, int width, int height, int format, int showMs);
void review_retag(const char *name, const char *tag);
bool review_request(int showMs);
void review_poll(void);
void review_show_for(int ms);
void review_hide(void);
bool review_showing(u64 now);
const char *review_name(void);
const char *review_tag(void);
void review_flash_shutter(void);
u64 review_shutter_flash_at(void);
