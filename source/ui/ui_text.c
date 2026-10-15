#include "ui_text.h"
#include <3ds.h>
#include <citro2d.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

// citro2d sizes every font like the system font, whose cells are this tall.
#define SYSTEM_FONT_CELL_HEIGHT 30.0f
#define SYSTEM_FONT_SCALE 0.5f // when no font is loaded
#define FONT_CACHE_SIZE 3 // one entry per loaded font
#define TEXT_DEPTH 0.9f
#define FORMATTED_TEXT_SIZE 128
#define WRAPPED_LINE_SIZE 96
#define SHADOW_OFFSET 1
#define SHADOW_COLOR UI_COLOR(0, 0, 0, 170)

C2D_TextBuf g_textBuffer;
C2D_Font g_fontSmall; // about 10 px
C2D_Font g_fontBody; // about 15 px
C2D_Font g_fontLarge; // about 26 px

float ui_text_font_scale(C2D_Font font)
{
	static C2D_Font cachedFonts[FONT_CACHE_SIZE];
	static float cachedScales[FONT_CACHE_SIZE];
	for (int i = 0; i < FONT_CACHE_SIZE; i++) {
		if (cachedFonts[i] == font && cachedScales[i] > 0) {
			return cachedScales[i];
		}
	}
	if (!font) {
		return SYSTEM_FONT_SCALE;
	}
	float scale = C2D_FontGetInfo(font)->tglp->cellHeight / SYSTEM_FONT_CELL_HEIGHT;
	for (int i = 0; i < FONT_CACHE_SIZE; i++) {
		if (!cachedFonts[i]) {
			cachedFonts[i] = font;
			cachedScales[i] = scale;
			break;
		}
	}
	return scale;
}

float ui_text_draw_string(C2D_Font font, float x, float y, float scale, u32 color, int align, float maxWidth,
                          const char *text)
{
	scale *= ui_text_font_scale(font);
	C2D_Text parsed;
	C2D_TextFontParse(&parsed, font, g_textBuffer, text);
	C2D_TextOptimize(&parsed);
	float width;
	float height;
	C2D_TextGetDimensions(&parsed, scale, scale, &width, &height);
	if (maxWidth > 0 && width > maxWidth) {
		scale *= maxWidth / width;
		width = maxWidth;
	}
	if (align == TEXT_ALIGN_CENTER) {
		x -= width * 0.5f;
	} else if (align == TEXT_ALIGN_RIGHT) {
		x -= width;
	}
	C2D_DrawText(&parsed, C2D_WithColor, x, y, TEXT_DEPTH, scale, scale, color);
	return width;
}

float ui_text_draw(C2D_Font font, float x, float y, u32 color, int align, const char *format, ...)
{
	char text[FORMATTED_TEXT_SIZE];
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(text, sizeof text, format, arguments);
	va_end(arguments);
	return ui_text_draw_string(font, x, y, 1.0f, color, align, 0, text);
}

float ui_text_draw_shadowed(C2D_Font font, float x, float y, u32 color, int align, const char *format, ...)
{
	char text[FORMATTED_TEXT_SIZE];
	va_list arguments;
	va_start(arguments, format);
	vsnprintf(text, sizeof text, format, arguments);
	va_end(arguments);
	return ui_text_draw_shadowed_string(font, x, y, color, align, 0, text);
}

float ui_text_draw_shadowed_string(C2D_Font font, float x, float y, u32 color, int align, float maxWidth,
                                   const char *text)
{
	ui_text_draw_string(font, x + SHADOW_OFFSET, y + SHADOW_OFFSET, 1.0f, SHADOW_COLOR, align, maxWidth,
	                    text);
	return ui_text_draw_string(font, x, y, 1.0f, color, align, maxWidth, text);
}

void ui_text_draw_wrapped(C2D_Font font, float x, float y, float lineHeight, int maxChars, int maxLines,
                          u32 color, const char *text)
{
	char line[WRAPPED_LINE_SIZE];
	for (int lineIndex = 0; lineIndex < maxLines && *text; lineIndex++) {
		int length = (int)strlen(text);
		if (length > maxChars) {
			// Break at the last space that fits. A word longer than a line is cut.
			length = maxChars;
			while (length > 0 && text[length] != ' ') {
				length--;
			}
			if (!length) {
				length = maxChars;
			}
		}
		snprintf(line, sizeof line, "%.*s", length, text);
		ui_text_draw_string(font, x, y + lineIndex * lineHeight, 1.0f, color, TEXT_ALIGN_LEFT, 0, line);
		text += length;
		while (*text == ' ') {
			text++;
		}
	}
}
