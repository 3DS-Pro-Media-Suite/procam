// Fonts, the colour palette and text drawing.
#pragma once
#include <3ds.h>
#include <citro2d.h>

// UTF-8 signs for texts.
#define UTF8_PLUS_MINUS_SIGN "\xC2\xB1"
#define UTF8_MULTIPLICATION_SIGN "\xC3\x97"

enum {
	TEXT_ALIGN_LEFT,
	TEXT_ALIGN_CENTER,
	TEXT_ALIGN_RIGHT
};

#define UI_COLOR(red, green, blue, alpha) C2D_Color32(red, green, blue, alpha)
#define COLOR_BLACK UI_COLOR(0, 0, 0, 255)
#define COLOR_INK UI_COLOR(238, 235, 228, 255)
#define COLOR_INK_SOFT UI_COLOR(210, 207, 200, 255) // list text that is not selected
#define COLOR_INK_DIM UI_COLOR(180, 177, 170, 255) // help text
#define COLOR_MUTED UI_COLOR(136, 133, 128, 255)
#define COLOR_FAINT UI_COLOR(74, 73, 72, 255)
#define COLOR_LINE UI_COLOR(40, 40, 42, 255)
#define COLOR_TILE UI_COLOR(22, 22, 25, 255)
#define COLOR_TILE_SELECTED UI_COLOR(48, 37, 16, 255)
#define COLOR_ACCENT UI_COLOR(255, 178, 56, 255)
#define COLOR_RED UI_COLOR(255, 76, 64, 255)
#define COLOR_BLUE UI_COLOR(98, 166, 255, 255)
#define COLOR_SHADE UI_COLOR(0, 0, 0, 200)

extern C2D_TextBuf g_textBuffer;
extern C2D_Font g_fontSmall;
extern C2D_Font g_fontBody;
extern C2D_Font g_fontLarge;

// The scale that draws the font at its native pixel size.
float ui_text_font_scale(C2D_Font font);

// Draws text and returns its width. A positive maxWidth shrinks wider text to fit.
float ui_text_draw_string(C2D_Font font, float x, float y, float scale, u32 color, int align, float maxWidth,
                          const char *text);

// Draws formatted text at the font's native size and returns its width.
float ui_text_draw(C2D_Font font, float x, float y, u32 color, int align, const char *format, ...);

// As ui_text_draw, with a dark shadow that keeps the text readable on a picture.
float ui_text_draw_shadowed(C2D_Font font, float x, float y, u32 color, int align, const char *format, ...);

// As ui_text_draw_string at the native size, with the shadow.
float ui_text_draw_shadowed_string(C2D_Font font, float x, float y, u32 color, int align, float maxWidth,
                                   const char *text);

// Draws text broken at spaces into lines of at most maxChars characters.
void ui_text_draw_wrapped(C2D_Font font, float x, float y, float lineHeight, int maxChars, int maxLines,
                          u32 color, const char *text);
