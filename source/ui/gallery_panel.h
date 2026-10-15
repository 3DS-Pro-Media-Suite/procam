// The gallery's bottom screen: its buttons, delete and the FTP switch.
#pragma once
#include <3ds.h>

void gallery_panel_draw(void);
// down: the keys pressed this frame. touch: the touch position when KEY_TOUCH is among them.
void gallery_panel_input(u32 down, touchPosition touch);
