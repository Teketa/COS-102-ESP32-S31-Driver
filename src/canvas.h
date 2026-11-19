// RGB888 drawing surface (200x30) that effects render into. Converted to the
// panel's 16-bit-per-channel framebuffer with gamma + brightness at commit.
// Also the source for the web UI's live preview.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "framebuf.h"

#define CW PANEL_W
#define CH PANEL_H

typedef struct { uint8_t r, g, b; } rgb_t;

// Double buffered across cores: effects draw into g_canvas (content task,
// core 0) while the display task packs and sends g_canvas_show (core 1).
// canvas_publish() copies draw -> show; a copy rather than a pointer swap so
// that read-modify-write effects (fire, matrix, comet...) still see their own
// previous frame in g_canvas.
extern rgb_t g_canvas[CH][CW];       // draw target
extern rgb_t g_canvas_show[CH][CW];  // display source (panel + web preview)
void canvas_publish(void);
void canvas_publish_scaled(uint16_t m);   // publish with brightness x m/256

void canvas_clear(void);
void canvas_fill(rgb_t c);
void canvas_fade(uint8_t amount);            // scale all pixels toward black
static inline void canvas_px(int x, int y, rgb_t c)
{
    if ((unsigned)x < CW && (unsigned)y < CH) g_canvas[y][x] = c;
}
void canvas_blend(int x, int y, rgb_t c, uint8_t alpha);   // alpha 0..255
void canvas_add(int x, int y, rgb_t c);                     // additive
void canvas_hline(int y, int x0, int x1, rgb_t c);
void canvas_vline(int x, int y0, int y1, rgb_t c);
void canvas_rect(int x, int y, int w, int h, rgb_t c, bool fill);

// Text: draws UTF-8 string at (x, y) top-left, integer scale, returns width.
int  canvas_text(int x, int y, const char *s, int scale, rgb_t c);
// Fractional-x text (smooth scrolling); blends across two panel columns.
int  canvas_text_sub(float fx, int y, const char *s, int scale, rgb_t c);
// Per-character color callback variant (for rainbow/wave text effects).
typedef rgb_t (*char_color_fn)(int char_index, int px, int py, void *ctx);
int  canvas_text_cb(int x, int y, const char *s, int scale,
                    char_color_fn fn, void *ctx);
// Draw one glyph column-accurately (used by scroll animations).
void canvas_char(int x, int y, uint32_t cp, int scale, rgb_t c);

rgb_t hsv(uint8_t h, uint8_t s, uint8_t v);
rgb_t rgb_scale(rgb_t c, uint8_t v);
static inline rgb_t RGB(uint8_t r, uint8_t g, uint8_t b) { rgb_t c = {r, g, b}; return c; }

// Commit: canvas -> gamma+brightness -> g_fb (does not send).
void canvas_commit(uint8_t brightness);
// Fused, LUT-driven canvas -> driver slots (render fast path).
void canvas_pack_slots(uint8_t brightness);
uint32_t canvas_hash(void);   // change detection (skip re-sending identical frames)
uint8_t rnd8(void);
uint32_t rnd32(void);
