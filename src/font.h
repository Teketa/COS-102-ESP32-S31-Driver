// 5x7 bitmap font, ASCII 32..126 plus Danish Æ Ø Å æ ø å (UTF-8 aware).
#pragma once
#include <stdint.h>

#define FONT_W 5
#define FONT_H 7
#define FONT_GAP 1              // column gap between glyphs
#define FONT_ADVANCE (FONT_W + FONT_GAP)

// Glyph column data (5 bytes, bit0 = top pixel) for a decoded codepoint.
const uint8_t *font_glyph(uint32_t cp);
// Decode one UTF-8 codepoint; advances *s past it.
uint32_t utf8_next(const char **s);
// Width in pixels of a UTF-8 string at scale 1 (no trailing gap).
int font_text_width(const char *s);
// Number of visible characters (UTF-8 aware).
int font_text_len(const char *s);
