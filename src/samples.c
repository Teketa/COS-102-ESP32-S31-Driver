#include "samples.h"
#include "beatsync.h"
#include "canvas.h"
#include "font.h"
#include <math.h>
#include <string.h>
#include "esp_timer.h"

samples_cfg_t g_samples;

const char *const g_sample_style_names[SMP_STYLE_COUNT] = {
    "Solid", "Beat flash", "Rainbow", "Strobe",
    "Marquee", "Letter by letter", "Drop in", "Punch", "Wave", "Glitch",
};

void samples_defaults(void)
{
    memset(&g_samples, 0, sizeof(g_samples));
    g_samples.version = SAMPLES_CFG_VERSION;
    g_samples.enabled = true;
    for (int i = 0; i < SAMPLE_SLOTS; i++) {
        g_samples.slot[i].rgb[0] = 255;
        g_samples.slot[i].rgb[1] = 255;
        g_samples.slot[i].rgb[2] = 255;
        g_samples.slot[i].style = SMP_SOLID;
    }
}

#define PREVIEW_US 2500000
static int s_prev_n;
static int64_t s_prev_t;

void samples_preview(int n)
{
    if (n < 1 || n > SAMPLE_SLOTS) return;
    s_prev_t = esp_timer_get_time();
    s_prev_n = n;
}

const sample_slot_t *samples_active(int *sampler, uint32_t *ms)
{
    // A preview from the web UI wins for its 2.5 s, even with Mixxx running.
    if (s_prev_n) {
        int64_t d = esp_timer_get_time() - s_prev_t;
        const sample_slot_t *p = &g_samples.slot[s_prev_n - 1];
        if (d < PREVIEW_US && p->text[0]) {
            if (sampler) *sampler = s_prev_n;
            if (ms) *ms = (uint32_t)(d / 1000);
            return p;
        }
        if (d >= PREVIEW_US) s_prev_n = 0;
    }
    if (!g_samples.enabled) return NULL;
    uint32_t order[64];
    int64_t start[64];
    uint64_t m = beatsync_samplers(order, start);
    if (!m) return NULL;
    int best = -1;
    for (int i = 0; i < SAMPLE_SLOTS; i++) {
        if (!(m >> i & 1) || !g_samples.slot[i].text[0]) continue;
        if (best < 0 || order[i] > order[best]) best = i;
    }
    if (best < 0) return NULL;
    if (sampler) *sampler = best + 1;
    if (ms) {
        int64_t d = (esp_timer_get_time() - start[best]) / 1000;
        *ms = d < 0 ? 0 : (uint32_t)d;
    }
    return &g_samples.slot[best];
}

// ---------------------------------------------------------------- helpers
typedef struct { uint32_t st; } rb_ctx_t;

static rgb_t rainbow_cb(int idx, int px, int py, void *ctx)
{
    rb_ctx_t *c = ctx;
    return hsv((uint8_t)(idx * 32 + c->st / 8), 255, 255);
}

static uint8_t u8f(float v) { return v <= 0 ? 0 : (v >= 1 ? 255 : (uint8_t)(v * 255.0f + 0.5f)); }

static rgb_t mix(rgb_t a, rgb_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return RGB((uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
               (uint8_t)(a.b + (b.b - a.b) * t));
}

static uint32_t hash32(uint32_t h)
{
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h;
}

// Largest whole-pixel scale (1..4) whose text fits in max_h rows; whole
// pixels keep the text sharp.
static int fit_scale(const char *text, int max_h)
{
    int w1 = font_text_width(text), scale = 4;
    while (scale > 1 && (w1 * scale > CW - 2 || FONT_H * scale > max_h)) scale--;
    return scale;
}

// Bounce easing (0..1 -> 0..1, overshoots back like a dropped ball).
static float ease_bounce(float p)
{
    if (p < 1 / 2.75f) return 7.5625f * p * p;
    if (p < 2 / 2.75f) { p -= 1.5f / 2.75f; return 7.5625f * p * p + 0.75f; }
    if (p < 2.5f / 2.75f) { p -= 2.25f / 2.75f; return 7.5625f * p * p + 0.9375f; }
    p -= 2.625f / 2.75f;
    return 7.5625f * p * p + 0.984375f;
}

// Chasing lights around the panel edge, as in the Marquee text animation.
static void marquee_border(uint32_t ms)
{
    int o = (int)((ms / 90) % 4);
    rgb_t bc = hsv((uint8_t)(ms / 25), 255, 255);
    for (int i = o; i < CW; i += 4) { canvas_px(i, 0, bc); canvas_px(CW - 1 - i, CH - 1, bc); }
    for (int i = o; i < CH; i += 4) { canvas_px(0, CH - 1 - i, bc); canvas_px(CW - 1, i, bc); }
}

// ----------------------------------------------------------------- render
void samples_render(const sample_slot_t *s, uint32_t st, uint32_t ms)
{
    canvas_clear();
    const char *text = s->text;
    int style = s->style < SMP_STYLE_COUNT ? s->style : SMP_SOLID;
    // Styles that move letters vertically or draw a border get some headroom.
    int max_h = (style == SMP_WAVE || style == SMP_DROP) ? CH - 6
              : (style == SMP_MARQUEE) ? CH - 2 : CH;
    int scale = fit_scale(text, max_h);
    int w = font_text_width(text) * scale, h = FONT_H * scale;
    int x = (CW - w) / 2, y = (CH - h) / 2;
    int adv = FONT_ADVANCE * scale;
    rgb_t c = RGB(s->rgb[0], s->rgb[1], s->rgb[2]);
    double fr = g_bf.beats - floor(g_bf.beats);

    switch (style) {
    case SMP_BEAT:      // full on the beat, falling to 35 % before the next
        canvas_text(x, y, text, scale, rgb_scale(c, (uint8_t)(90 + 165 * expf(-3.0f * (float)fr))));
        break;
    case SMP_RAINBOW: {
        rb_ctx_t ctx = { st };
        canvas_text_cb(x, y, text, scale, rainbow_cb, &ctx);
        break;
    }
    case SMP_STROBE:    // ~8 Hz on/off
        if ((ms / 60) & 1) canvas_text(x, y, text, scale, c);
        break;
    case SMP_MARQUEE:
        canvas_text(x, y, text, scale, c);
        marquee_border(ms);
        break;
    case SMP_REVEAL: {  // letters pop in one by one, flashing white as they land
        const char *p = text;
        int i = 0, shown = 0;
        while (*p) {
            uint32_t cp = utf8_next(&p);
            uint32_t at = (uint32_t)i * 90;
            if (ms >= at) {
                float age = (ms - at) / 120.0f;
                canvas_char(x + i * adv, y, cp, scale, mix(RGB(255, 255, 255), c, age));
                shown++;
            }
            i++;
        }
        if (shown < i && (ms / 70) & 1)          // blinking cursor while typing
            canvas_rect(x + shown * adv, y, scale, h, c, true);
        break;
    }
    case SMP_DROP: {    // letters fall in one after another and bounce
        const char *p = text;
        int i = 0;
        while (*p) {
            uint32_t cp = utf8_next(&p);
            float t = ((float)ms - i * 70.0f) / 380.0f;
            if (t > 0) {
                float e = t >= 1 ? 1.0f : ease_bounce(t);
                int yy = (int)(-h - 2 + (y + h + 2) * e + 0.5f);
                canvas_char(x + i * adv, yy, cp, scale, c);
            }
            i++;
        }
        break;
    }
    case SMP_PUNCH: {   // white flash + shake that settles into the colour
        float p = ms / 450.0f;
        if (p < 1) {
            float amp = (1 - p) * (1 - p);
            int dx = (int)roundf(sinf(ms * 0.19f) * 5.0f * amp);
            int dy = (int)roundf(cosf(ms * 0.23f) * 1.5f * amp);
            if (ms < 70) canvas_fill(RGB(70, 70, 70));      // camera-flash pop
            canvas_text(x + dx, y + dy, text, scale, mix(RGB(255, 255, 255), c, p * 1.6f));
        } else {
            canvas_text(x, y, text, scale, c);
        }
        break;
    }
    case SMP_WAVE: {    // letters ride a wave
        const char *p = text;
        int i = 0;
        while (*p) {
            uint32_t cp = utf8_next(&p);
            int dy = (int)roundf(sinf(ms * 0.011f - i * 0.75f) * 2.6f);
            canvas_char(x + i * adv, y + dy, cp, scale, c);
            i++;
        }
        break;
    }
    case SMP_GLITCH: {  // digital tearing: RGB split, sliced rows, dropouts
        uint32_t slot = ms / 55;
        uint32_t hs = hash32(slot * 2654435761u + 17);
        bool split = (hs & 3) != 0;
        if (split) {                              // red and cyan ghosts either side
            int d = 1 + (int)(hs >> 4 & 1);
            canvas_text(x - d, y, text, scale, RGB(c.r, 0, 0));
            canvas_text(x + d, y, text, scale, RGB(0, c.g, c.b));
        }
        canvas_text(x, y, text, scale, (hs >> 8 & 7) == 0 ? RGB(255, 255, 255) : c);
        for (int k = 0; k < 3; k++) {             // shift a few row slices sideways
            uint32_t hk = hash32(hs + k * 977);
            if (hk & 1) continue;
            int r0 = (int)(hk >> 8) % CH, rows = 1 + (int)(hk >> 16 & 3);
            int shift = (int)(hk >> 20 & 15) - 7;
            for (int r = r0; r < r0 + rows && r < CH; r++) {
                rgb_t row[CW];
                memcpy(row, g_canvas[r], sizeof(row));
                for (int xx = 0; xx < CW; xx++) {
                    int src = xx - shift;
                    g_canvas[r][xx] = (src >= 0 && src < CW) ? row[src] : RGB(0, 0, 0);
                }
            }
        }
        break;
    }
    default:
        canvas_text(x, y, text, scale, c);
        break;
    }
}
