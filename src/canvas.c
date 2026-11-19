#include "canvas.h"
#include "font.h"
#include "protocol.h"
#include <string.h>
#include <math.h>

rgb_t g_canvas[CH][CW];
rgb_t g_canvas_show[CH][CW];

void canvas_publish(void) { memcpy(g_canvas_show, g_canvas, sizeof(g_canvas)); }

// Publish with every channel scaled by m/256 (beat pulse). The draw canvas is
// untouched, so effects that read back their previous frame aren't affected.
void canvas_publish_scaled(uint16_t m)
{
    const uint8_t *src = (const uint8_t *)g_canvas;
    uint8_t *dst = (uint8_t *)g_canvas_show;
    for (size_t i = 0; i < sizeof(g_canvas); i++)
        dst[i] = (uint8_t)((src[i] * (uint32_t)m) >> 8);
}

static uint16_t s_gamma[256];      // 8-bit sRGB-ish -> 16-bit linear PWM
static uint32_t s_rng = 0x12345678;

__attribute__((constructor))
static void gamma_init(void)
{
    for (int i = 0; i < 256; i++) {
        float f = powf(i / 255.0f, 2.2f);
        s_gamma[i] = (uint16_t)(f * 65535.0f + 0.5f);
    }
}

uint32_t rnd32(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
uint8_t rnd8(void) { return (uint8_t)(rnd32() >> 24); }

void canvas_clear(void) { memset(g_canvas, 0, sizeof(g_canvas)); }

void canvas_fill(rgb_t c)
{
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) g_canvas[y][x] = c;
}

void canvas_fade(uint8_t amount)
{
    uint16_t keep = 255 - amount;
    rgb_t *p = &g_canvas[0][0];
    for (int i = 0; i < CW * CH; i++, p++) {
        p->r = (uint8_t)((p->r * keep) >> 8);
        p->g = (uint8_t)((p->g * keep) >> 8);
        p->b = (uint8_t)((p->b * keep) >> 8);
    }
}

void canvas_blend(int x, int y, rgb_t c, uint8_t a)
{
    if ((unsigned)x >= CW || (unsigned)y >= CH) return;
    rgb_t *p = &g_canvas[y][x];
    p->r = (uint8_t)((p->r * (255 - a) + c.r * a) / 255);
    p->g = (uint8_t)((p->g * (255 - a) + c.g * a) / 255);
    p->b = (uint8_t)((p->b * (255 - a) + c.b * a) / 255);
}

void canvas_add(int x, int y, rgb_t c)
{
    if ((unsigned)x >= CW || (unsigned)y >= CH) return;
    rgb_t *p = &g_canvas[y][x];
    int r = p->r + c.r, g = p->g + c.g, b = p->b + c.b;
    p->r = r > 255 ? 255 : r;
    p->g = g > 255 ? 255 : g;
    p->b = b > 255 ? 255 : b;
}

void canvas_hline(int y, int x0, int x1, rgb_t c)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) canvas_px(x, y, c);
}

void canvas_vline(int x, int y0, int y1, rgb_t c)
{
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) canvas_px(x, y, c);
}

void canvas_rect(int x, int y, int w, int h, rgb_t c, bool fill)
{
    if (fill) {
        for (int j = y; j < y + h; j++) canvas_hline(j, x, x + w - 1, c);
    } else {
        canvas_hline(y, x, x + w - 1, c);
        canvas_hline(y + h - 1, x, x + w - 1, c);
        canvas_vline(x, y, y + h - 1, c);
        canvas_vline(x + w - 1, y, y + h - 1, c);
    }
}

void canvas_char(int x, int y, uint32_t cp, int scale, rgb_t c)
{
    const uint8_t *g = font_glyph(cp);
    for (int col = 0; col < FONT_W; col++) {
        for (int row = 0; row < FONT_H; row++) {
            if (!(g[col] & (1 << row))) continue;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    canvas_px(x + col * scale + sx, y + row * scale + sy, c);
        }
    }
}

int canvas_text(int x, int y, const char *s, int scale, rgb_t c)
{
    int x0 = x;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        canvas_char(x, y, cp, scale, c);
        x += FONT_ADVANCE * scale;
    }
    return x - x0;
}

// Sub-pixel horizontal text: draws at a fractional x by splitting each source
// column's intensity across the two neighbouring panel columns. Integer pixel
// steps make scrolling judder (at 60 fps a 0.76 px/frame scroll advances
// 1,1,0,1,1,0...); with sub-pixel positioning the motion is continuous, so it
// stays smooth at any speed and any frame rate.
int canvas_text_sub(float fx, int y, const char *s, int scale, rgb_t c)
{
    int xi = (int)floorf(fx);
    float frac = fx - (float)xi;
    uint8_t w2 = (uint8_t)(frac * 255.0f + 0.5f);
    uint8_t w1 = 255 - w2;
    rgb_t c1 = { (uint8_t)(c.r * w1 / 255), (uint8_t)(c.g * w1 / 255), (uint8_t)(c.b * w1 / 255) };
    rgb_t c2 = { (uint8_t)(c.r * w2 / 255), (uint8_t)(c.g * w2 / 255), (uint8_t)(c.b * w2 / 255) };
    int x = xi;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        const uint8_t *g = font_glyph(cp);
        for (int col = 0; col < FONT_W; col++) {
            for (int row = 0; row < FONT_H; row++) {
                if (!(g[col] & (1 << row))) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx, py = y + row * scale + sy;
                        if (w1) canvas_add(px, py, c1);
                        if (w2) canvas_add(px + 1, py, c2);
                    }
            }
        }
        x += FONT_ADVANCE * scale;
    }
    return x - xi;
}

int canvas_text_cb(int x, int y, const char *s, int scale,
                   char_color_fn fn, void *ctx)
{
    int x0 = x, idx = 0;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        const uint8_t *g = font_glyph(cp);
        for (int col = 0; col < FONT_W; col++) {
            for (int row = 0; row < FONT_H; row++) {
                if (!(g[col] & (1 << row))) continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + col * scale + sx, py = y + row * scale + sy;
                        canvas_px(px, py, fn(idx, px, py, ctx));
                    }
            }
        }
        x += FONT_ADVANCE * scale;
        idx++;
    }
    return x - x0;
}

rgb_t hsv(uint8_t h, uint8_t s, uint8_t v)
{
    rgb_t out;
    uint8_t region = h / 43;
    uint8_t rem = (h - region * 43) * 6;
    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * rem) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - rem)) >> 8))) >> 8;
    switch (region) {
    case 0:  out = (rgb_t){v, t, p}; break;
    case 1:  out = (rgb_t){q, v, p}; break;
    case 2:  out = (rgb_t){p, v, t}; break;
    case 3:  out = (rgb_t){p, q, v}; break;
    case 4:  out = (rgb_t){t, p, v}; break;
    default: out = (rgb_t){v, p, q}; break;
    }
    return out;
}

rgb_t rgb_scale(rgb_t c, uint8_t v)
{
    return (rgb_t){ (uint8_t)((c.r * v) >> 8),
                    (uint8_t)((c.g * v) >> 8),
                    (uint8_t)((c.b * v) >> 8) };
}

uint32_t canvas_hash(void)
{
    // FNV-1a over the canvas. Callers mix in anything else that affects the
    // panel output (brightness, mode).
    const uint8_t *p = (const uint8_t *)g_canvas;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sizeof(g_canvas); i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h ? h : 1;
}

// Fused canvas -> driver slots: brightness + gamma via one 256-entry table and
// the precomputed pixel-mapping LUT. Replaces canvas_commit()+fb_pack() on the
// render path (skips the intermediate 16-bit framebuffer entirely).
void canvas_pack_slots(uint8_t brightness)
{
    static uint16_t bl[256];
    static int last_bri = -1;
    if (brightness != last_bri) {
        for (int i = 0; i < 256; i++) bl[i] = s_gamma[i * brightness / 255];
        last_bri = brightness;
    }
    const uint16_t *lut = map_lut();
    const rgb_t *cv = &g_canvas_show[0][0];   // published frame, not the draft
    for (int s = 0; s < SRAM_SLOTS; s++) {
        uint16_t *r = g_slots[s].r, *g = g_slots[s].g, *b = g_slots[s].b;
        const uint16_t *L = lut + s * WORDS_PER_SHIFT;
        for (int w = 0; w < WORDS_PER_SHIFT; w++) {
            uint16_t idx = L[w];
            if (idx == 0xFFFF) { r[w] = g[w] = b[w] = 0; continue; }
            rgb_t c = cv[idx];
            r[w] = bl[c.r]; g[w] = bl[c.g]; b[w] = bl[c.b];
        }
    }
    if (g_map.word_lsb_first) {          // debug option; slow path
        for (int s = 0; s < SRAM_SLOTS; s++)
            for (int w = 0; w < WORDS_PER_SHIFT; w++) {
                uint16_t *p[3] = { &g_slots[s].r[w], &g_slots[s].g[w], &g_slots[s].b[w] };
                for (int k = 0; k < 3; k++) {
                    uint16_t v = *p[k], o = 0;
                    for (int i = 0; i < 16; i++) if (v & (1 << i)) o |= 0x8000 >> i;
                    *p[k] = o;
                }
            }
    }
}

void canvas_commit(uint8_t brightness)
{
    for (int y = 0; y < CH; y++) {
        for (int x = 0; x < CW; x++) {
            rgb_t c = g_canvas[y][x];
            uint8_t r = (uint8_t)((c.r * brightness) / 255);
            uint8_t g = (uint8_t)((c.g * brightness) / 255);
            uint8_t b = (uint8_t)((c.b * brightness) / 255);
            fb_set(x, y, s_gamma[r], s_gamma[g], s_gamma[b]);
        }
    }
}
