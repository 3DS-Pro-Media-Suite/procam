#include "screens.h"
#include <3ds.h>
#include <citro2d.h>
#include "gallery.h"
#include "button_hints.h"
#include "input.h"
#include "look_editor.h"
#include "menus.h"
#include "settings.h"
#include "ui_layout.h"
#include "ui_text.h"
#include "value_picker.h"
#include "viewfinder.h"

// Moves a bottom-screen-wide picture to the middle of the top screen.
#define SCREEN_CENTER_OFFSET_X ((TOP_SCREEN_WIDTH - BOTTOM_SCREEN_WIDTH) / 2)

// Swapped screens apply only while shooting: the gallery, the menus and the picker keep the touch screen.
static bool screens_swapped_now(void)
{
	return g_screensSwapped && !gallery_is_open() && !g_menuOpen && !g_infoOpen && !value_picker_active() &&
	       !look_editor_active();
}

void screens_draw_frame(const Screens *screens)
{
	C2D_TextBufClear(g_textBuffer);
	// Stereo output only while a 3D picture is on screen.
	bool want3D = gallery_is_open() && gallery_state()->stereoShown;
	if (want3D != gfxIs3D()) {
		gfxSet3D(want3D);
	}
	C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
	bool swapped = screens_swapped_now();
	C2D_TargetClear(screens->top, COLOR_BLACK);
	C2D_SceneBegin(screens->top);
	if (swapped) {
		// The controls in the middle of the top screen, the 4:3 viewfinder filling the touch screen.
		C2D_ViewTranslate(SCREEN_CENTER_OFFSET_X, 0);
		menus_draw();
		C2D_ViewReset();
		C2D_TargetClear(screens->bottom, COLOR_BLACK);
		C2D_SceneBegin(screens->bottom);
		C2D_ViewTranslate(-SCREEN_CENTER_OFFSET_X, 0);
		viewfinder_set_inset(SCREEN_CENTER_OFFSET_X);
	}
	viewfinder_draw(GFX_LEFT);
	if (g_showControls && !gallery_is_open() && !g_menuOpen) {
		button_hints_draw();
	}
	if (want3D && !swapped) {
		C2D_TargetClear(screens->topRight, COLOR_BLACK);
		C2D_SceneBegin(screens->topRight);
		viewfinder_draw(GFX_RIGHT);
		C2D_SceneBegin(screens->top);
	}
	viewfinder_draw_camera_status();
	if (swapped) {
		viewfinder_set_inset(0);
		C2D_ViewReset();
	} else {
		C2D_TargetClear(screens->bottom, COLOR_BLACK);
		C2D_SceneBegin(screens->bottom);
		menus_draw();
	}
	C3D_FrameEnd(0);
}
