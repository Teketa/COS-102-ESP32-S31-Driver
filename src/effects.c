#include "effects.h"
#include "canvas.h"
#include "font.h"
#include "settings.h"
#include "render.h"
#include "beatsync.h"
#include "effects_art.h"
#include "effects_theme.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <time.h>

static rgb_t FG(void) { return RGB(g_set.fg[0], g_set.fg[1], g_set.fg[2]); }
static rgb_t BG(void) { return RGB(g_set.bg[0], g_set.bg[1], g_set.bg[2]); }

// Integer sine table (phase 0..255 -> -127..127). The per-pixel effects used
// sinf()/sqrtf() thousands of times per frame, which blew the frame budget and
// made the vsync pacing irregular (visible as stutter + brightness pulsing).
static int8_t SIN[256];

__attribute__((constructor))
static void sin_init(void)
{
    for (int i = 0; i < 256; i++)
        SIN[i] = (int8_t)(sinf(i * 2.0f * (float)M_PI / 256.0f) * 127.0f);
}

// Octagonal distance approximation: max + min/2, ~4% error, no sqrt.
static inline int dist_approx(int dx, int dy)
{
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;
    return dx > dy ? dx + (dy >> 1) : dy + (dx >> 1);
}

// Shared per-effect state (union keeps RAM to one effect's worth).
#define NSTARS 70
#define NBALLS 8
#define NPART  90
#define NROCK  4
#define NSPARK 12

// --- European pharmacy sign geometry ---------------------------------------
// A Greek cross: arm reach PH_R from centre, bar thickness PH_T. Sized to the
// 30-pixel panel height with a pixel of margin, and repeated PH_N times across
// the 200-pixel width.
#define PH_R 13
#define PH_T 9
#define PH_N 3
#define PH_SPACING (CW / PH_N)

static union {
    uint8_t heat[CH][CW];
    struct { int16_t drop[CW]; uint8_t len[CW]; uint8_t spd[CW]; uint32_t step; } mtx;
    struct { float x, y, z; } stars[NSTARS];
    struct { float y, v; uint8_t hue; } balls[NBALLS];
    struct { float x, y, vx, vy, life; uint8_t hue; } part[NPART];
    struct { float x, y, vy; uint8_t hue; bool live; } rocks[NROCK];
    struct { float x, y, vy; uint8_t hue; } snow[NSTARS];
    struct { uint8_t ang[2 * PH_R + 1][2 * PH_R + 1]; } cross;
    struct { uint8_t x[NSPARK], y[NSPARK]; } spk;
    struct { int32_t last; uint8_t h[20]; } bb;
    struct { float level, peak; } vu;
} S;
static bool s_init;

// ------------------------------------------------------ frame-locked motion
// Anything with a hard edge that travels (a line, a bar edge, scrolling text)
// must move by the same whole number of pixels every frame, or it judders.
// Deriving the position from the clock (x = st / 25) gives 40 px/s, which at
// 78 fps is 0.51 px/frame: the line steps 1,0,1,0,1,1,0... and that uneven
// rhythm reads as a pulsing stutter. Sub-pixel blending hides it but blurs
// the edge. So these effects advance on a fixed frame cadence instead:
// n pixels every frame, or 1 pixel every n frames -- whichever is closest to
// the requested speed at the current frame rate. The speed slider still
// scales it, and pause freezes it.
static int32_t s_mpos;       // whole pixels travelled since the effect started
static uint32_t s_mcnt;      // frame counter for the "1 px every n frames" case

// Call exactly once per frame from an effect that uses s_mpos.
// `px_per_s` is the nominal speed at the default speed setting (50).
static void motion_advance(float px_per_s)
{
    if (g_set.paused) return;
    int fps = render_fps_effective();
    if (fps < 1) fps = 1;
    float v = px_per_s * render_speed() / (float)fps;
    if (v >= 1.0f) {
        s_mpos += (int32_t)(v + 0.5f);
        s_mcnt = 0;
    } else {
        uint32_t n = (uint32_t)(1.0f / v + 0.5f);
        if (++s_mcnt >= n) { s_mcnt = 0; s_mpos++; }
    }
}

// Back-and-forth position over 0..len-1 (no double dwell at the ends).
static int pingpong(int32_t p, int len)
{
    int period = 2 * (len - 1);
    int q = (int)(((p % period) + period) % period);
    return q < len ? q : period - q;
}

// ------------------------------------------------ speed-scaled frame steps
// Effects that simulate something per frame (falling snow, bouncing balls,
// fading trails, random spawns) were tuned at 30 fps on the S3, and ran at a
// speed proportional to the frame rate -- 2.6x faster at 78 fps, with the
// speed slider and pause doing nothing. They now scale each frame's step by
// s_k = how many 30-fps frames of animation this frame stands for:
//     s_k = 30 / fps * speed / 50     (0 while paused)
// It is the same amount every frame (not measured frame-to-frame), so the
// motion stays even.
#define REF_FPS 30.0f
static float s_k;            // reference frames of animation in this frame
static float s_acc;          // fractional ticks carried between frames
static int32_t s_mprev;      // s_mpos at the previous frame (meteor drift)

// Whole events due this frame for a rate of `per_ref_frame` per 30-fps frame.
static int ticks(float per_ref_frame)
{
    s_acc += per_ref_frame * s_k;
    int n = (int)s_acc;
    s_acc -= (float)n;
    return n;
}

// True with probability `p_per_ref_frame` per 30-fps frame.
static bool chance(float p_per_ref_frame)
{
    return (float)(rnd32() & 0xFFFF) < p_per_ref_frame * s_k * 65536.0f;
}

// canvas_fade(amount) as tuned per 30-fps frame, compounded for s_k frames.
static void fade_k(uint8_t amount)
{
    if (s_k <= 0.0f) return;                     // paused: hold the image
    float keep = powf((255 - amount) / 256.0f, s_k);
    int a = (int)(255.0f - keep * 256.0f + 0.5f);
    canvas_fade((uint8_t)(a < 0 ? 0 : (a > 255 ? 255 : a)));
}

void fx_reset(void)
{
    memset(&S, 0, sizeof(S));
    s_init = false;
    s_mpos = 0;
    s_mcnt = 0;
    s_mprev = 0;
    s_acc = 0;
    art_reset();
    canvas_clear();
}

// ---------------------------------------------------------------- effects
static void fx_solid(uint32_t st, uint32_t f) { canvas_fill(FG()); }

static void fx_rainbow(uint32_t st, uint32_t f)
{
    for (int x = 0; x < CW; x++) {
        rgb_t c = hsv((uint8_t)(x * 256 / CW - st / 16), 255, 255);
        canvas_vline(x, 0, CH - 1, c);
    }
}

static void fx_rainbow_cycle(uint32_t st, uint32_t f)
{
    canvas_fill(hsv((uint8_t)(st / 16), 255, 255));
}

static void fx_plasma(uint32_t st, uint32_t f)
{
    uint8_t t1 = (uint8_t)(st / 10);
    uint8_t t2 = (uint8_t)((st * 7) / 100);
    uint8_t t3 = (uint8_t)((st * 13) / 100);
    for (int y = 0; y < CH; y++) {
        int dy = (y - CH / 2) * 3;
        for (int x = 0; x < CW; x++) {
            int d = dist_approx(x - CW / 2, dy);
            int v = SIN[(uint8_t)(x * 5 + t1)]
                  + SIN[(uint8_t)(y * 8 - t2)]
                  + SIN[(uint8_t)((x + y) * 4 + t3)]
                  + SIN[(uint8_t)(d * 7 - t1)];
            canvas_px(x, y, hsv((uint8_t)((v + 512) >> 2), 255, 255));
        }
    }
}

static void fx_fire_step(void)
{
    for (int x = 0; x < CW; x++) {
        int v = S.heat[CH - 1][x] + (rnd8() % 90) - 40;
        S.heat[CH - 1][x] = v < 0 ? 0 : (v > 255 ? 255 : v);
    }
    for (int y = 0; y < CH - 1; y++)
        for (int x = 0; x < CW; x++) {
            int l = S.heat[y + 1][(x + CW - 1) % CW];
            int c = S.heat[y + 1][x];
            int r = S.heat[y + 1][(x + 1) % CW];
            int v = (l + c * 2 + r) / 4 - 8 - (rnd8() % 6);
            S.heat[y][x] = v < 0 ? 0 : v;
        }
}

static void fx_fire(uint32_t st, uint32_t f)
{
    int n = ticks(1.0f);                         // one step per 30-fps frame
    if (n > 4) n = 4;
    while (n--) fx_fire_step();
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            uint8_t h = S.heat[y][x];
            canvas_px(x, y, hsv(h / 6, 255, h));   // black->red->orange->yellow
        }
}

static void fx_matrix(uint32_t st, uint32_t f)
{
    if (!s_init) {
        for (int x = 0; x < CW; x++) {
            S.mtx.drop[x] = -(int16_t)(rnd8() % 60);
            S.mtx.len[x] = 4 + rnd8() % 12;
            S.mtx.spd[x] = 1 + rnd8() % 3;      // per-column fall speed
        }
        S.mtx.step = st / 55;
        s_init = true;
    }
    // Advance on a time grid (not per frame) and redraw the whole scene every
    // frame: fading on frames that draw nothing makes the effect strobe.
    uint32_t step = st / 55;
    while ((int32_t)(step - S.mtx.step) > 0) {
        S.mtx.step++;
        for (int x = 0; x < CW; x++) {
            if (S.mtx.step % S.mtx.spd[x]) continue;
            if (++S.mtx.drop[x] > CH + S.mtx.len[x]) {
                S.mtx.drop[x] = -(int16_t)(rnd8() % 40);
                S.mtx.len[x] = 4 + rnd8() % 12;
                S.mtx.spd[x] = 1 + rnd8() % 3;
            }
        }
    }
    canvas_clear();
    for (int x = 0; x < CW; x++) {
        int head = S.mtx.drop[x], len = S.mtx.len[x];
        canvas_px(x, head, RGB(200, 255, 200));
        for (int i = 1; i < len; i++)
            canvas_px(x, head - i, RGB(0, (uint8_t)(255 - i * 255 / len), 0));
    }
}

static void fx_starfield(uint32_t st, uint32_t f)
{
    if (!s_init) {
        for (int i = 0; i < NSTARS; i++) {
            S.stars[i].x = (float)(rnd32() % 2000) / 10.0f - 100.0f;
            S.stars[i].y = (float)(rnd32() % 600) / 10.0f - 30.0f;
            S.stars[i].z = (float)(rnd32() % 1000) / 1000.0f * 2.0f + 0.2f;
        }
        s_init = true;
    }
    canvas_clear();
    for (int i = 0; i < NSTARS; i++) {
        S.stars[i].z -= 0.02f * s_k;
        if (S.stars[i].z <= 0.05f) {
            S.stars[i].x = (float)(rnd32() % 2000) / 10.0f - 100.0f;
            S.stars[i].y = (float)(rnd32() % 600) / 10.0f - 30.0f;
            S.stars[i].z = 2.2f;
        }
        int x = (int)(S.stars[i].x / S.stars[i].z) + CW / 2;
        int y = (int)(S.stars[i].y / S.stars[i].z) + CH / 2;
        uint8_t b = (uint8_t)(255 - (uint8_t)(S.stars[i].z * 100));
        canvas_px(x, y, RGB(b, b, b));
    }
}

static void fx_twinkle(uint32_t st, uint32_t f)
{
    fade_k(12);
    for (int i = ticks(3.0f); i > 0; i--) {
        int x = rnd32() % CW, y = rnd32() % CH;
        canvas_px(x, y, hsv(rnd8(), 120, 255));
    }
}

static void fx_sparkle(uint32_t st, uint32_t f)
{
    // New sparkle positions once per 30-fps frame's worth of time; the same
    // positions are redrawn in between.
    if (!s_init || ticks(1.0f) > 0) {
        for (int i = 0; i < NSPARK; i++) {
            S.spk.x[i] = (uint8_t)(rnd32() % CW);
            S.spk.y[i] = (uint8_t)(rnd32() % CH);
        }
        s_init = true;
    }
    canvas_fill(BG());
    for (int i = 0; i < NSPARK; i++) canvas_px(S.spk.x[i], S.spk.y[i], FG());
}

static void fx_confetti(uint32_t st, uint32_t f)
{
    fade_k(18);
    for (int i = ticks(4.0f); i > 0; i--)
        canvas_px(rnd32() % CW, rnd32() % CH, hsv(rnd8(), 200, 255));
}

// Brightness along a trail: one level per pixel behind the head, drawn from
// the head's path (not by fading the previous frame), so the trail looks the
// same at any frame rate. `decay` is the per-pixel keep factor out of 256.
static void trail_levels(uint8_t *lvl, int len, int32_t head, int trail, int decay)
{
    memset(lvl, 0, len);
    uint32_t b = 255;
    for (int k = 0; k < trail && b; k++) {
        int x = pingpong(head - k, len);
        if (b > lvl[x]) lvl[x] = (uint8_t)b;     // the fold at the ends keeps the max
        b = b * (uint32_t)decay / 256;
    }
}

static void fx_comet(uint32_t st, uint32_t f)
{
    static uint8_t lvl[CW];
    motion_advance(40.0f);                       // was 1 px per 25 ms
    trail_levels(lvl, CW, s_mpos, 40, 230);
    canvas_clear();
    rgb_t fg = FG();
    for (int x = 0; x < CW; x++)
        if (lvl[x]) canvas_vline(x, 0, CH - 1, rgb_scale(fg, lvl[x]));
}

static void fx_theater(uint32_t st, uint32_t f)
{
    motion_advance(11.0f);                       // was one step per 90 ms
    int o = (int)(s_mpos % 3);
    canvas_fill(BG());
    for (int x = o; x < CW; x += 3)
        for (int y = 0; y < CH; y++) canvas_px(x, y, FG());
}

static void fx_wipe(uint32_t st, uint32_t f)
{
    motion_advance(50.0f);                       // was 1 px per 20 ms
    int p = (int)(s_mpos % (CW * 2));
    rgb_t a = p < CW ? FG() : BG(), b = p < CW ? BG() : FG();
    int e = p % CW;
    for (int x = 0; x < CW; x++) canvas_vline(x, 0, CH - 1, x <= e ? a : b);
}

static void fx_breathe(uint32_t st, uint32_t f)
{
    float v = (sinf(st / 500.0f) + 1.0f) * 0.5f;
    canvas_fill(rgb_scale(FG(), (uint8_t)(v * 255)));
}

static void fx_gradient(uint32_t st, uint32_t f)
{
    rgb_t a = FG(), b = BG();
    for (int x = 0; x < CW; x++) {
        int m = x * 255 / (CW - 1);
        rgb_t c = { (uint8_t)((a.r * (255 - m) + b.r * m) / 255),
                    (uint8_t)((a.g * (255 - m) + b.g * m) / 255),
                    (uint8_t)((a.b * (255 - m) + b.b * m) / 255) };
        canvas_vline(x, 0, CH - 1, c);
    }
}

static void fx_balls(uint32_t st, uint32_t f)
{
    if (!s_init) {
        for (int i = 0; i < NBALLS; i++) {
            S.balls[i].y = (float)(rnd32() % (CH * 10)) / 10.0f;
            S.balls[i].v = 0;
            S.balls[i].hue = rnd8();
        }
        s_init = true;
    }
    fade_k(60);
    for (int i = 0; i < NBALLS; i++) {
        S.balls[i].v += 0.06f * s_k;
        S.balls[i].y += S.balls[i].v * s_k;
        if (S.balls[i].y >= CH - 1) { S.balls[i].y = CH - 1; S.balls[i].v *= -0.86f; }
        int x = (i + 1) * CW / (NBALLS + 1);
        rgb_t c = hsv(S.balls[i].hue, 255, 255);
        for (int d = -1; d <= 1; d++) canvas_px(x + d, (int)S.balls[i].y, c);
        canvas_px(x, (int)S.balls[i].y - 1, c);
    }
}

static void fx_snow(uint32_t st, uint32_t f)
{
    if (!s_init) {
        for (int i = 0; i < NSTARS; i++) {
            S.snow[i].x = (float)(rnd32() % (CW * 10)) / 10.0f;
            S.snow[i].y = (float)(rnd32() % (CH * 10)) / 10.0f;
            S.snow[i].vy = 0.1f + (rnd8() % 40) / 200.0f;
        }
        s_init = true;
    }
    canvas_clear();
    for (int i = 0; i < NSTARS; i++) {
        S.snow[i].y += S.snow[i].vy * s_k;
        S.snow[i].x += sinf((st / 600.0f) + i) * 0.06f * s_k;
        if (S.snow[i].y >= CH) { S.snow[i].y = 0; S.snow[i].x = (float)(rnd32() % (CW * 10)) / 10.0f; }
        uint8_t b = (uint8_t)(140 + S.snow[i].vy * 400);
        canvas_px((int)S.snow[i].x, (int)S.snow[i].y, RGB(b, b, 255));
    }
}

static void fx_fireworks(uint32_t st, uint32_t f)
{
    fade_k(40);
    // launch
    if (chance(1.0f / 24.0f)) {
        for (int i = 0; i < NPART; i++) {
            if (S.part[i].life > 0) continue;
            float cx = 10 + rnd32() % (CW - 20), cy = 4 + rnd32() % (CH - 10);
            uint8_t hue = rnd8();
            int n = 0;
            for (int j = 0; j < NPART && n < 22; j++) {
                if (S.part[j].life > 0) continue;
                float a = (float)(rnd32() % 628) / 100.0f;
                float sp = 0.15f + (rnd8() % 60) / 200.0f;
                S.part[j] = (typeof(S.part[0])){ cx, cy, cosf(a) * sp, sinf(a) * sp, 1.0f, hue };
                n++;
            }
            break;
        }
    }
    for (int i = 0; i < NPART; i++) {
        if (S.part[i].life <= 0) continue;
        S.part[i].x += S.part[i].vx * s_k;
        S.part[i].y += S.part[i].vy * s_k;
        S.part[i].vy += 0.012f * s_k;
        S.part[i].life -= 0.022f * s_k;
        if (S.part[i].life < 0) S.part[i].life = 0;
        canvas_add((int)S.part[i].x, (int)S.part[i].y,
                   hsv(S.part[i].hue, 200, (uint8_t)(S.part[i].life * 255)));
    }
}

static void fx_wave(uint32_t st, uint32_t f)
{
    canvas_clear();
    for (int x = 0; x < CW; x++) {
        float v = sinf(x / 10.0f - st / 300.0f) * (CH / 2 - 2) + CH / 2.0f;
        int y = (int)v;
        rgb_t c = hsv((uint8_t)(x * 2 - st / 20), 255, 255);
        canvas_px(x, y, c);
        canvas_px(x, y + 1, rgb_scale(c, 100));
        canvas_px(x, y - 1, rgb_scale(c, 100));
    }
}

static void fx_ripple(uint32_t st, uint32_t f)
{
    uint8_t t = (uint8_t)(st / 5);
    rgb_t fg = FG();
    for (int y = 0; y < CH; y++) {
        int dy = (y - CH / 2) * 3;
        for (int x = 0; x < CW; x++) {
            int d = dist_approx(x - CW / 2, dy);
            uint8_t b = (uint8_t)(SIN[(uint8_t)(d * 14 - t)] + 128);
            canvas_px(x, y, rgb_scale(fg, b));
        }
    }
}

static void fx_scanner(uint32_t st, uint32_t f)
{
    static uint8_t lvl[CW];
    motion_advance(83.0f);                       // was 1 px per 12 ms
    trail_levels(lvl, CW, s_mpos, 36, 236);
    int p = pingpong(s_mpos, CW);
    for (int d = -2; d <= 2; d++) {              // soft 5-px head
        int x = p + d;
        uint8_t b = (uint8_t)(255 >> (d < 0 ? -d : d));
        if (x >= 0 && x < CW && b > lvl[x]) lvl[x] = b;
    }
    canvas_clear();
    rgb_t fg = FG();
    for (int x = 0; x < CW; x++)
        if (lvl[x]) canvas_vline(x, 0, CH - 1, rgb_scale(fg, lvl[x]));
}

static void fx_checker(uint32_t st, uint32_t f)
{
    int o = (st / 200) % 8;
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++)
            canvas_px(x, y, (((x + o) / 4 + y / 4) & 1) ? FG() : BG());
}

static void fx_bars(uint32_t st, uint32_t f)
{
    canvas_fill(BG());
    int nb = 20, bw = CW / nb;
    for (int b = 0; b < nb; b++) {
        float v = (sinf(st / 300.0f + b * 0.7f) + sinf(st / 170.0f + b * 1.9f)) * 0.25f + 0.5f;
        int h = (int)(v * CH);
        for (int y = CH - h; y < CH; y++) {
            rgb_t c = hsv((uint8_t)(85 - (CH - y) * 85 / CH), 255, 255);
            canvas_hline(y, b * bw, b * bw + bw - 2, c);
        }
    }
}

static void fx_clouds(uint32_t st, uint32_t f)
{
    uint8_t t1 = (uint8_t)(st / 17);
    uint8_t t2 = (uint8_t)((st * 6) / 170);
    uint8_t t3 = (uint8_t)((st * 4) / 170);
    rgb_t fg = FG();
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            int a = (SIN[(uint8_t)(x * 3 + t1)] * SIN[(uint8_t)(y * 6 - t2)]) / 127;
            int v = a + SIN[(uint8_t)((x + y) * 2 + t3)];
            canvas_px(x, y, rgb_scale(fg, (uint8_t)((v + 256) >> 1)));
        }
}

static void fx_radar(uint32_t st, uint32_t f)
{
    fade_k(24);
    float a = st / 400.0f;
    for (float r = 0; r < CW / 2; r += 0.5f) {
        int x = (int)(CW / 2 + cosf(a) * r);
        int y = (int)(CH / 2 + sinf(a) * r * 0.3f);
        canvas_px(x, y, rgb_scale(FG(), (uint8_t)(255 - r * 2)));
    }
}

static void fx_rings(uint32_t st, uint32_t f)
{
    canvas_clear();
    for (int k = 0; k < 4; k++) {
        float r = fmodf(st / 60.0f + k * 12.0f, 50.0f);
        uint8_t b = (uint8_t)(255 - r * 5);
        for (int deg = 0; deg < 360; deg += 4) {
            float rad = deg * 3.14159f / 180.0f;
            canvas_add((int)(CW / 2 + cosf(rad) * r),
                       (int)(CH / 2 + sinf(rad) * r * 0.32f),
                       hsv((uint8_t)(k * 60 + st / 30), 255, b));
        }
    }
}

static void fx_meteor(uint32_t st, uint32_t f)
{
    fade_k(30);
    if (chance(0.1f)) {
        int y = rnd32() % CH;
        for (int i = 0; i < 14; i++)
            canvas_px(CW - 1 - i, y + i / 6, rgb_scale(hsv(rnd8() % 40 + 20, 180, 255),
                                                       (uint8_t)(255 - i * 16)));
    }
    // drift existing content left, on the frame-locked cadence (was 1 px
    // every frame, i.e. 30 px/s at 30 fps)
    motion_advance(30.0f);
    int shift = (int)(s_mpos - s_mprev);
    s_mprev = s_mpos;
    if (shift > CW) shift = CW;
    if (shift > 0)
        for (int y = 0; y < CH; y++) {
            memmove(&g_canvas[y][0], &g_canvas[y][shift], (CW - shift) * sizeof(rgb_t));
            memset(&g_canvas[y][CW - shift], 0, shift * sizeof(rgb_t));
        }
}

static void fx_aurora(uint32_t st, uint32_t f)
{
    float t = st / 900.0f;
    for (int x = 0; x < CW; x++) {
        float base = sinf(x / 18.0f + t) * 5 + sinf(x / 7.0f - t * 1.4f) * 3 + CH / 2;
        for (int y = 0; y < CH; y++) {
            float d = fabsf(y - base);
            uint8_t b = d > 9 ? 0 : (uint8_t)(255 * (1.0f - d / 9.0f));
            canvas_px(x, y, hsv((uint8_t)(96 + sinf(x / 20.0f + t) * 40), 255, b));
        }
    }
}

static void fx_pride(uint32_t st, uint32_t f)
{
    for (int y = 0; y < CH; y++) {
        rgb_t c = hsv((uint8_t)(y * 256 / CH + st / 20), 255, 255);
        canvas_hline(y, 0, CW - 1, c);
    }
}

static void fx_dna(uint32_t st, uint32_t f)
{
    canvas_clear();
    for (int x = 0; x < CW; x++) {
        float p = x / 9.0f - st / 250.0f;
        int y1 = (int)(CH / 2 + sinf(p) * (CH / 2 - 2));
        int y2 = (int)(CH / 2 - sinf(p) * (CH / 2 - 2));
        if (x % 6 == 0) canvas_vline(x, y1 < y2 ? y1 : y2, y1 < y2 ? y2 : y1,
                                     rgb_scale(FG(), 60));
        canvas_px(x, y1, hsv((uint8_t)(st / 20), 255, 255));
        canvas_px(x, y2, hsv((uint8_t)(st / 20 + 128), 255, 255));
    }
}

// --------------------------------------------------- pharmacy cross effects
// The classic green cross seen on European pharmacies. The animations below
// are the traditional ones: a rotating sweep, a breathing pulse, a chase
// around the outline, arms building outward, and the plain on/off blink.

static const rgb_t PH_GREEN = { 0, 255, 70 };     // vivid pharmacy green
static const rgb_t PH_DIM   = { 0, 70, 20 };

static rgb_t lerp_rgb(rgb_t a, rgb_t b, uint8_t t)
{
    return (rgb_t){ (uint8_t)(a.r + (b.r - a.r) * t / 255),
                    (uint8_t)(a.g + (b.g - a.g) * t / 255),
                    (uint8_t)(a.b + (b.b - a.b) * t / 255) };
}

// Is (dx,dy) inside a Greek cross with arm reach r and half-thickness h?
static inline bool in_cross(int dx, int dy, int r, int h)
{
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    return (ax <= r && ay <= h) || (ay <= r && ax <= h);
}

// Precompute the angle of every pixel around the cross centre once, so the
// rotating animations cost a table lookup instead of atan2f per pixel.
static void cross_init(void)
{
    for (int y = -PH_R; y <= PH_R; y++)
        for (int x = -PH_R; x <= PH_R; x++) {
            float a = atan2f((float)y, (float)x) + (float)M_PI;   // 0..2pi
            S.cross.ang[y + PH_R][x + PH_R] =
                (uint8_t)(a * 255.0f / (2.0f * (float)M_PI));
        }
    s_init = true;
}

static int cross_cx(int k) { return PH_SPACING / 2 + k * PH_SPACING; }

// Rotating sweep: a bright wedge orbits the centre of each cross.
static void fx_pharmacy(uint32_t st, uint32_t f)
{
    if (!s_init) cross_init();
    canvas_clear();
    uint8_t phase = (uint8_t)(st / 7);
    for (int k = 0; k < PH_N; k++) {
        int cx = cross_cx(k);
        uint8_t kp = (uint8_t)(phase + k * 40);     // stagger the crosses
        for (int dy = -PH_R; dy <= PH_R; dy++)
            for (int dx = -PH_R; dx <= PH_R; dx++) {
                if (!in_cross(dx, dy, PH_R, PH_T / 2)) continue;
                uint8_t d = (uint8_t)(S.cross.ang[dy + PH_R][dx + PH_R] - kp);
                uint8_t w = d < 64 ? (uint8_t)(255 - d * 4) : 0;   // trailing wedge
                canvas_px(cx + dx, CH / 2 + dy, lerp_rgb(PH_DIM, PH_GREEN, w));
            }
    }
}

// Breathing pulse: brightness and arm reach swell together.
static void fx_cross_pulse(uint32_t st, uint32_t f)
{
    canvas_clear();
    for (int k = 0; k < PH_N; k++) {
        uint8_t ph = (uint8_t)(st / 8 + k * 85);
        uint8_t v = (uint8_t)(SIN[ph] + 128);
        int r = 7 + (int)(v * (PH_R - 7) / 255);
        int h = PH_T / 2;
        rgb_t c = rgb_scale(PH_GREEN, (uint8_t)(90 + v * 165 / 255));
        int cx = cross_cx(k);
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
                if (in_cross(dx, dy, r, h)) canvas_px(cx + dx, CH / 2 + dy, c);
    }
}

// Chase: bright dots run around the outline of each cross.
static void fx_cross_chase(uint32_t st, uint32_t f)
{
    if (!s_init) cross_init();
    canvas_clear();
    uint8_t phase = (uint8_t)(st / 5);
    const int h = PH_T / 2;
    for (int k = 0; k < PH_N; k++) {
        int cx = cross_cx(k);
        for (int dy = -PH_R; dy <= PH_R; dy++)
            for (int dx = -PH_R; dx <= PH_R; dx++) {
                if (!in_cross(dx, dy, PH_R, h)) continue;
                // Outline = a cross pixel with at least one non-cross neighbour
                bool edge = !in_cross(dx + 1, dy, PH_R, h) ||
                            !in_cross(dx - 1, dy, PH_R, h) ||
                            !in_cross(dx, dy + 1, PH_R, h) ||
                            !in_cross(dx, dy - 1, PH_R, h);
                rgb_t c = PH_DIM;
                if (edge) {
                    uint8_t d = (uint8_t)(S.cross.ang[dy + PH_R][dx + PH_R] - phase);
                    uint8_t w = d < 48 ? (uint8_t)(255 - d * 5) : 0;
                    c = lerp_rgb(rgb_scale(PH_GREEN, 110), RGB(200, 255, 210), w);
                }
                canvas_px(cx + dx, CH / 2 + dy, c);
            }
    }
}

// Build: centre first, then each arm extends outward, then a full flash.
static void fx_cross_build(uint32_t st, uint32_t f)
{
    canvas_clear();
    const int h = PH_T / 2;
    uint32_t step = (st / 90) % (uint32_t)(PH_R * 4 + 10);
    for (int k = 0; k < PH_N; k++) {
        int cx = cross_cx(k), cy = CH / 2;
        bool flash = step >= (uint32_t)(PH_R * 4) && ((step / 2) & 1);
        rgb_t c = flash ? RGB(190, 255, 200) : PH_GREEN;
        for (int dy = -PH_R; dy <= PH_R; dy++)
            for (int dx = -PH_R; dx <= PH_R; dx++) {
                if (!in_cross(dx, dy, PH_R, h)) continue;
                int reach;                       // how far out this pixel sits
                if (dy < -h)      reach = -dy;   // up arm
                else if (dx > h)  reach = dx;    // right arm
                else if (dy > h)  reach = dy;    // down arm
                else if (dx < -h) reach = -dx;   // left arm
                else              reach = 0;     // centre block
                int arm = dy < -h ? 0 : (dx > h ? 1 : (dy > h ? 2 : (dx < -h ? 3 : -1)));
                bool on = flash || arm < 0 ||
                          (int)step >= arm * PH_R + reach;
                if (on) canvas_px(cx + dx, cy + dy, c);
            }
    }
}

// Blink: the plainest pharmacy sign — cross on, then a frame around it.
static void fx_cross_blink(uint32_t st, uint32_t f)
{
    canvas_clear();
    bool alt = ((st / 500) & 1);
    const int h = PH_T / 2;
    for (int k = 0; k < PH_N; k++) {
        int cx = cross_cx(k);
        if (!alt) {
            for (int dy = -PH_R; dy <= PH_R; dy++)
                for (int dx = -PH_R; dx <= PH_R; dx++)
                    if (in_cross(dx, dy, PH_R, h))
                        canvas_px(cx + dx, CH / 2 + dy, PH_GREEN);
        } else {
            canvas_rect(cx - PH_R, CH / 2 - PH_R, 2 * PH_R + 1, 2 * PH_R + 1,
                        PH_GREEN, false);
        }
    }
}

// ------------------------------------------------------------ beat effects
// Driven by g_bf.beats, the beat position at the moment this frame is shown:
// from Mixxx through the bridge when connected, otherwise a free-running
// 120 BPM clock (scaled by the speed slider). Integer part = beat number,
// fraction = position within the beat.
static int32_t bnum(void) { return (int32_t)floor(g_bf.beats); }
static float bfrac(void) { return (float)(g_bf.beats - floor(g_bf.beats)); }
static float benv(float k) { return expf(-k * bfrac()); }   // 1 on the beat, decays
static uint8_t u8f(float v) { return v <= 0 ? 0 : (v >= 1 ? 255 : (uint8_t)(v * 255.0f + 0.5f)); }

// Whole panel flashes the text colour on every beat and fades to background.
static void fx_beat_flash(uint32_t st, uint32_t f)
{
    canvas_fill(lerp_rgb(BG(), FG(), u8f(benv(5.0f))));
}

// Short hard strobe on every beat (first 8 % of the beat).
static void fx_beat_strobe(uint32_t st, uint32_t f)
{
    canvas_fill(bfrac() < 0.08f ? FG() : BG());
}

// A new colour on every beat, fading down to a glow.
static void fx_beat_colors(uint32_t st, uint32_t f)
{
    canvas_fill(hsv((uint8_t)(bnum() * 47), 255, u8f(0.25f + 0.75f * benv(3.0f))));
}

// Four blocks, one per beat of the bar; the colour moves on every bar.
static void fx_beat_chase(uint32_t st, uint32_t f)
{
    int32_t n = bnum();
    int seg = (int)(((n % 4) + 4) % 4);
    uint8_t hue = (uint8_t)((n >> 2) * 64);
    uint8_t lit = u8f(0.15f + 0.85f * benv(2.5f));
    int w = CW / 4;
    canvas_clear();
    for (int k = 0; k < 4; k++)
        canvas_rect(k * w, 0, w - 2, CH, hsv(hue, 255, k == seg ? lit : 20), true);
}

// A ring bursts out from the centre on every beat.
static void fx_beat_ripple(uint32_t st, uint32_t f)
{
    float fr = bfrac();
    int r = (int)(fr * 110.0f);
    rgb_t c = hsv((uint8_t)(bnum() * 53), 230, u8f(1.0f - fr));
    canvas_clear();
    for (int y = 0; y < CH; y++) {
        int dy = (y - CH / 2) * 3;
        for (int x = 0; x < CW; x++) {
            int d = dist_approx(x - CW / 2, dy) - r;
            if (d < 0) d = -d;
            if (d < 4) canvas_px(x, y, rgb_scale(c, (uint8_t)(255 - d * 60)));
        }
    }
}

// Equaliser look: every beat throws the bars to new random heights, which
// fall back during the beat.
static void fx_beat_bars(uint32_t st, uint32_t f)
{
    int32_t n = bnum();
    if (!s_init || n != S.bb.last) {
        S.bb.last = n;
        for (int i = 0; i < 20; i++) S.bb.h[i] = (uint8_t)(90 + rnd8() % 166);
        s_init = true;
    }
    float e = benv(2.0f);
    int nb = 20, bw = CW / nb;
    canvas_clear();
    for (int b = 0; b < nb; b++) {
        int h = (int)(S.bb.h[b] / 255.0f * CH * e + 0.5f);
        if (h < 1) h = 1;
        for (int y = CH - h; y < CH; y++)
            canvas_hline(y, b * bw, b * bw + bw - 2,
                         hsv((uint8_t)(85 - (CH - y) * 85 / CH), 255, 255));
    }
}

// A new colour wipes across the panel over each bar (4 beats). The leading
// column is blended so the edge glides instead of stepping unevenly.
static void fx_bar_wipe(uint32_t st, uint32_t f)
{
    double bars = g_bf.beats / 4.0;
    int32_t bar = (int32_t)floor(bars);
    float pos = (float)(bars - bar) * CW;
    int edge = (int)pos;
    rgb_t cur = hsv((uint8_t)(bar * 71), 255, 255);
    rgb_t prev = hsv((uint8_t)((bar - 1) * 71), 255, 255);
    for (int x = 0; x < CW; x++) {
        rgb_t c = x < edge ? cur : prev;
        if (x == edge) c = lerp_rgb(prev, cur, u8f(pos - edge));
        canvas_vline(x, 0, CH - 1, c);
    }
}

// Mixxx's main output level, mirrored from the centre, with falling peak
// markers and a beat accent on the top and bottom rows. Without Mixxx it
// bounces on the free-running beat instead.
static void fx_vu_meter(uint32_t st, uint32_t f)
{
    float lvl = g_bf.synced ? g_bf.vu : 0.75f * benv(3.0f);
    // fast attack, slower release; peak hold falls slowly
    S.vu.level = lvl > S.vu.level ? lvl : S.vu.level - 0.05f * s_k;
    if (S.vu.level < 0) S.vu.level = 0;
    S.vu.peak = S.vu.level > S.vu.peak ? S.vu.level : S.vu.peak - 0.008f * s_k;
    if (S.vu.peak < 0) S.vu.peak = 0;
    const int half = CW / 2;
    int n = (int)(S.vu.level * half + 0.5f);
    canvas_clear();
    for (int i = 0; i < n && i < half; i++) {
        float t = (float)i / half;
        rgb_t c = t < 0.6f ? RGB(0, 255, 40) : (t < 0.85f ? RGB(255, 190, 0) : RGB(255, 30, 0));
        canvas_vline(half + i, 3, CH - 4, c);
        canvas_vline(half - 1 - i, 3, CH - 4, c);
    }
    int p = (int)(S.vu.peak * half + 0.5f);
    if (p > 0 && p <= half) {
        canvas_vline(half + p - 1, 3, CH - 4, RGB(255, 255, 255));
        canvas_vline(half - p, 3, CH - 4, RGB(255, 255, 255));
    }
    rgb_t acc = rgb_scale(FG(), u8f(benv(6.0f)));
    for (int y = 0; y < 2; y++) { canvas_hline(y, 0, CW - 1, acc); canvas_hline(CH - 1 - y, 0, CW - 1, acc); }
}

// ------------------------------------------------- more beat effects
// Stateless: everything is computed from the beat position (and a hash of
// the beat number for "random" choices), so these stay locked to the music
// through cue jumps, pause and speed changes, and need no per-effect memory.

// Beat position wrapped to 1024 beats for float maths. 1024 is a multiple of
// every cycle used below, so the wrap is seamless.
static float bt_f(void)
{
    double b = g_bf.beats;
    return (float)(b - 1024.0 * floor(b / 1024.0));
}

static uint32_t bhash(int32_t b, uint32_t salt)
{
    uint32_t h = (uint32_t)b * 2654435761u ^ salt * 0x9E3779B9u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return h;
}

// Radial lookup tables around the panel centre (vertical distances x3, the
// same stretch Beat Ripple uses so circles fill the wide panel), and angles
// from a point just below the bottom centre for the laser fan. Built once.
static uint8_t s_rad[CH][CW];    // 0..~110
static uint8_t s_ang[CH][CW];    // 0..255 = full turn around the centre
static uint8_t s_fan[CH][CW];    // 0..255 = far left .. far right, from below
static bool s_radial_ok;

static void radial_init(void)
{
    if (s_radial_ok) return;
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            float dx = x - (CW - 1) / 2.0f, dy = (y - (CH - 1) / 2.0f) * 3.0f;
            float r = sqrtf(dx * dx + dy * dy);
            s_rad[y][x] = (uint8_t)(r > 255 ? 255 : r);
            float a = atan2f(dy, dx);                        // -pi..pi
            s_ang[y][x] = (uint8_t)((int)((a + (float)M_PI) * 256.0f / (2.0f * (float)M_PI)) & 255);
            float fa = atan2f(dx, (CH + 4 - y) * 3.0f);      // -pi/2..pi/2 from below
            s_fan[y][x] = (uint8_t)((fa + (float)M_PI / 2) * 255.0f / (float)M_PI);
        }
    s_radial_ok = true;
}

// Elliptical ring (vertical x3), soft edge `width` px, added to the canvas.
// True distance (not dist_approx's octagon) so the rings come out round.
static void ring_add(int cx, int cy, float r, float width, rgb_t c)
{
    int reach = (int)(r + width) + 1;
    int x0 = cx - reach < 0 ? 0 : cx - reach;
    int x1 = cx + reach >= CW ? CW - 1 : cx + reach;
    for (int y = 0; y < CH; y++) {
        int dy = (y - cy) * 3;
        if (dy > reach || -dy > reach) continue;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)(x - cx);
            float d = fabsf(sqrtf(dx * dx + (float)(dy * dy)) - r);
            if (d < width) canvas_add(x, y, rgb_scale(c, (uint8_t)(255.0f * (1.0f - d / width))));
        }
    }
}

// Every beat drops a ring at a random spot; rings live 3 beats and overlap.
static void fx_ripple_rain(uint32_t st, uint32_t f)
{
    const int LIFE = 3;
    double beats = g_bf.beats;
    int32_t n = bnum();
    canvas_clear();
    for (int k = LIFE - 1; k >= 0; k--) {
        int32_t b = n - k;
        float age = (float)(beats - b);
        if (age < 0 || age >= LIFE) continue;
        uint32_t h = bhash(b, 1);
        int cx = 16 + (int)(h % (CW - 32)), cy = 5 + (int)((h >> 9) % (CH - 10));
        float fade = 1.0f - age / LIFE;
        ring_add(cx, cy, age * 28.0f, 3.5f, hsv((uint8_t)(h >> 20), 220, u8f(fade)));
        if (age < 0.15f)                             // the drop itself
            canvas_rect(cx - 1, cy - 1, 3, 3, hsv((uint8_t)(h >> 20), 120, u8f(1 - age / 0.15f)), true);
    }
}

// Rings alternate between the left and right thirds and cross in the middle.
static void fx_stereo_ripple(uint32_t st, uint32_t f)
{
    double beats = g_bf.beats;
    int32_t n = bnum();
    uint8_t hue = (uint8_t)((n >> 2) * 40);          // shifts every bar
    canvas_clear();
    for (int k = 1; k >= 0; k--) {
        int32_t b = n - k;
        float age = (float)(beats - b);
        if (age < 0 || age >= 2) continue;
        bool right = b & 1;
        int cx = right ? (3 * CW) / 4 : CW / 4;
        float fade = 1.0f - age / 2;
        ring_add(cx, CH / 2, age * 55.0f, 4.0f,
                 hsv((uint8_t)(hue + (right ? 128 : 0)), 230, u8f(fade)));
    }
}

// Endless rings flying out of the centre, two per beat, with a kick accent.
static void fx_ripple_tunnel(uint32_t st, uint32_t f)
{
    radial_init();
    float bt = bt_f();
    float accent = 0.55f + 0.45f * benv(4.0f);
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            float p = (s_rad[y][x] - bt * 36.0f) / 18.0f;   // ring coordinate
            float fl = floorf(p), fr = p - fl;
            float tri = 1.0f - fabsf(fr * 2.0f - 1.0f);      // peak mid-ring
            tri = tri * tri * tri;
            float depth = 0.25f + s_rad[y][x] / 140.0f;     // brighter as they come closer
            int ring = (int)fl;
            canvas_px(x, y, hsv((uint8_t)(ring * 37), 230, u8f(tri * depth * accent)));
        }
}

// A burst of sparks on every beat from a random point, falling under gravity.
// Each spark is drawn as a short streak from where it was a moment ago.
static void spark_pos(float cx, float cy, float vx, float vy, float age, float *x, float *y)
{
    float e = 1.0f - expf(-3.0f * age);              // sparks slow down
    *x = cx + vx * e;
    *y = cy + vy * e + 5.0f * age * age;             // and fall
}

static void fx_starburst(uint32_t st, uint32_t f)
{
    const int SPARKS = 44;
    double beats = g_bf.beats;
    int32_t n = bnum();
    canvas_clear();
    for (int k = 1; k >= 0; k--) {
        int32_t b = n - k;
        float age = (float)(beats - b);
        if (age < 0 || age >= 1.6f) continue;
        uint32_t h = bhash(b, 2);
        float cx = 25 + (float)(h % (CW - 50)), cy = 8 + (float)((h >> 8) % (CH - 16));
        uint8_t hue = (uint8_t)(h >> 16);
        float fade = 1.0f - age / 1.6f;
        if (age < 0.12f)                             // flash at the origin
            ring_add((int)cx, (int)cy, 0.0f, 9.0f * (1.0f - age / 0.12f) + 1.0f,
                     hsv(hue, 80, u8f(1.0f - age / 0.12f)));
        for (int j = 0; j < SPARKS; j++) {
            uint32_t hj = bhash(b * 64 + j, 3);
            uint8_t a = (uint8_t)(j * 256 / SPARKS + (hj & 7));
            float sp = 35.0f + (float)(hj >> 8 & 47);   // px per beat
            float vx = SIN[(uint8_t)(a + 64)] / 127.0f * sp;
            float vy = SIN[a] / 127.0f * sp * 0.35f;     // wide, flat bursts
            float x0, y0, x1, y1;
            spark_pos(cx, cy, vx, vy, age > 0.07f ? age - 0.07f : 0.0f, &x0, &y0);
            spark_pos(cx, cy, vx, vy, age, &x1, &y1);
            rgb_t col = hsv((uint8_t)(hue + (hj >> 16 & 31)), 200, u8f(fade));
            int steps = (int)(fabsf(x1 - x0) + fabsf(y1 - y0)) + 1;
            for (int t = 0; t <= steps; t++) {          // streak, brightest at the head
                float u = (float)t / steps;
                canvas_add((int)(x0 + (x1 - x0) * u + 0.5f), (int)(y0 + (y1 - y0) * u + 0.5f),
                           rgb_scale(col, (uint8_t)(90 + 165 * u)));
            }
        }
    }
}

// Mirrored six-fold kaleidoscope, turning with the tempo (a full turn every
// 4 bars), zooming in time and pulsing on the beat.
static void fx_kaleidoscope(uint32_t st, uint32_t f)
{
    radial_init();
    float bt = bt_f();
    uint8_t rot = (uint8_t)(int)(bt * 16.0f);        // a full turn per 4 bars of 4 beats
    uint8_t zoom = (uint8_t)(int)(bt * 64.0f);
    uint8_t hue0 = (uint8_t)(int)(bt * 6.0f);
    float pulse = 0.5f + 0.5f * benv(3.0f);
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            uint8_t seg = (uint8_t)(s_ang[y][x] * 6 + rot);
            int fold = seg < 128 ? seg * 2 : (255 - seg) * 2;   // mirror inside each slice
            int r = s_rad[y][x];
            int v = SIN[(uint8_t)(fold + r * 4 - zoom)] + SIN[(uint8_t)(r * 3 + fold / 2 + zoom)];
            if (v <= 0) { canvas_px(x, y, RGB(0, 0, 0)); continue; }
            uint8_t bri = (uint8_t)(v > 254 ? 255 : v + 1);
            canvas_px(x, y, hsv((uint8_t)(hue0 + fold / 3 + r), 235, (uint8_t)(bri * pulse)));
        }
}

// Laser beams fanning up from below the stage, sweeping once per bar and
// flashing on every beat; colours swap each beat.
static void fx_laser_fan(uint32_t st, uint32_t f)
{
    const int BEAMS = 7;
    radial_init();
    float bt = bt_f();
    float sweep = sinf(bt / 4.0f * 2.0f * (float)M_PI);     // one sweep per bar
    float spread = 26.0f + 10.0f * sinf(bt / 8.0f * 2.0f * (float)M_PI);
    float flash = 0.25f + 0.75f * benv(3.5f);
    int32_t n = bnum();
    canvas_clear();
    float centre = 128.0f + sweep * 40.0f;
    ring_add(CW / 2, CH + 1, 0.0f, 14.0f, hsv(200, 60, u8f(0.6f * flash)));   // source glow
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            float a = s_fan[y][x], best = 1e9f;
            int bi = 0;
            for (int i = 0; i < BEAMS; i++) {
                float d = fabsf(a - (centre + (i - BEAMS / 2) * spread));
                if (d < best) { best = d; bi = i; }
            }
            if (best > 4.0f) continue;
            float w = 1.0f - best / 4.0f;
            uint8_t hue = (uint8_t)(((n + bi) & 1) ? 150 : 230);   // blue / magenta
            float dist = 0.7f + 0.3f * (float)y / CH;              // brighter near the source
            canvas_px(x, y, hsv(hue, best < 1.0f ? 90 : 255, u8f(w * flash * dist)));
        }
}

// Balls that land exactly on the beat (odd ones on the off-beat).
static void fx_beat_bounce(uint32_t st, uint32_t f)
{
    const int BALLS = 10;
    double beats = g_bf.beats;
    canvas_clear();
    for (int i = 0; i < BALLS; i++) {
        double bb = beats + ((i & 1) ? 0.5 : 0.0);
        int32_t b = (int32_t)floor(bb);
        float fr = (float)(bb - b);
        uint32_t h = bhash(b, 10 + i);
        float hmax = 9.0f + (float)(h % 16);                 // jump height this beat
        float y = (CH - 4) - hmax * 4.0f * fr * (1.0f - fr);
        int cx = (2 * i + 1) * CW / (2 * BALLS);
        bool squash = fr < 0.06f || fr > 0.94f;
        rgb_t c = hsv((uint8_t)(i * 25 + (b >> 2) * 16), 230, 255);
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -3; dx <= 3; dx++) {
                float ex = dx / (squash ? 3.4f : 2.4f), ey = dy / (squash ? 1.6f : 2.4f);
                if (ex * ex + ey * ey <= 1.0f) canvas_px(cx + dx, (int)(y + 0.5f) + dy, c);
            }
    }
    rgb_t fl = rgb_scale(FG(), u8f(0.15f + 0.85f * benv(6.0f)));   // floor lights on landing
    canvas_hline(CH - 1, 0, CW - 1, fl);
}

// One eased sweep per beat, alternating direction, with a fading trail.
// The trail is sampled along the head's path; each sample fills the span back
// to the previous one, so a fast head leaves a solid trail, not a comb.
static void fx_beat_scanner(uint32_t st, uint32_t f)
{
    static uint8_t lvl[CW];
    memset(lvl, 0, sizeof(lvl));
    double beats = g_bf.beats;
    int prev = -1;
    for (int t = 30; t >= 0; t--) {                  // oldest first
        double bb = beats - t * 0.012;
        int32_t b = (int32_t)floor(bb);
        float fr = (float)(bb - b);
        float e = 0.5f - 0.5f * cosf(fr * (float)M_PI);      // ease in-out
        float pos = (b & 1) ? (1.0f - e) : e;
        int x = (int)(pos * (CW - 1) + 0.5f);
        uint8_t v = (uint8_t)(255 - t * 8);
        int a = prev < 0 ? x : prev, z = x;
        if (a > z) { int tmp = a; a = z; z = tmp; }
        for (int xx = a; xx <= z; xx++) if (v > lvl[xx]) lvl[xx] = v;
        for (int d = -1; d <= 1; d += 2) {           // soft edges on the head
            int xx = x + d;
            if (xx >= 0 && xx < CW && v / 2 > lvl[xx]) lvl[xx] = v / 2;
        }
        prev = x;
    }
    uint8_t hue = (uint8_t)(bnum() * 29);
    canvas_clear();
    for (int x = 0; x < CW; x++)
        if (lvl[x]) canvas_vline(x, 0, CH - 1, hsv(hue, 200, lvl[x]));
}

// Plasma that flows with the tempo and pumps on the kick.
static void fx_beat_plasma(uint32_t st, uint32_t f)
{
    float bt = bt_f();
    uint8_t t1 = (uint8_t)(int)(bt * 32.0f), t2 = (uint8_t)(int)(bt * 24.0f),
            t3 = (uint8_t)(int)(bt * 40.0f), hue0 = (uint8_t)(int)(bt * 8.0f);
    float pump = 0.35f + 0.65f * benv(2.5f);
    for (int y = 0; y < CH; y++) {
        int dy = (y - CH / 2) * 3;
        for (int x = 0; x < CW; x++) {
            int d = dist_approx(x - CW / 2, dy);
            int v = SIN[(uint8_t)(x * 4 + t1)] + SIN[(uint8_t)(y * 9 - t2)]
                  + SIN[(uint8_t)((x + y) * 3 + t3)] + SIN[(uint8_t)(d * 6 - t1 * 2)];
            canvas_px(x, y, hsv((uint8_t)(hue0 + ((v + 512) >> 2)), 240,
                                (uint8_t)(255 * pump)));
        }
    }
}

// Twenty segments; a random one flashes on every 16th note, four on the beat.
static void fx_sixteenths(uint32_t st, uint32_t f)
{
    const int SEG = 20, W = CW / SEG;
    double s16 = g_bf.beats * 4.0;
    int32_t n = (int32_t)floor(s16);
    canvas_clear();
    for (int k = 2; k >= 0; k--) {                   // this 16th and two fading ones
        int32_t q = n - k;
        float age = (float)(s16 - q);                // in 16ths
        float v = expf(-1.6f * age);
        bool on_beat = (q & 3) == 0;
        int count = on_beat ? 4 : 1;
        for (int c = 0; c < count; c++) {
            uint32_t h = bhash(q, 20 + c);
            int sgi = (int)(h % SEG);
            rgb_t col = on_beat ? RGB(255, 255, 255) : hsv((uint8_t)((q >> 4) * 48 + (h >> 24) % 32), 255, 255);
            col = rgb_scale(col, u8f(v));
            for (int y = 0; y < CH; y++)
                for (int x = sgi * W; x < sgi * W + W - 1; x++) canvas_add(x, y, col);
        }
    }
}

float fx_frame_k(void) { return s_k; }

// Frame-locked motion for effects in other files: call once per frame.
int32_t fx_motion(float px_per_s) { motion_advance(px_per_s); return s_mpos; }

const uint8_t *fx_radial_rad(void) { radial_init(); return &s_rad[0][0]; }
const uint8_t *fx_radial_ang(void) { radial_init(); return &s_ang[0][0]; }

int fx_next(int cur, int dir, bool beat_only)
{
    int n = g_effect_count;
    for (int k = 1; k <= n; k++) {
        int i = ((cur + dir * k) % n + n) % n;
        if (!beat_only || g_effects[i].beat) return i;
    }
    return cur;
}

const effect_t g_effects[] = {
    { "Rainbow",       fx_rainbow },
    { "Rainbow Cycle", fx_rainbow_cycle },
    { "Plasma",        fx_plasma },
    { "Fire",          fx_fire },
    { "Matrix Rain",   fx_matrix },
    { "Starfield",     fx_starfield },
    { "Twinkle",       fx_twinkle },
    { "Sparkle",       fx_sparkle },
    { "Confetti",      fx_confetti },
    { "Comet",         fx_comet },
    { "Theater Chase", fx_theater },
    { "Color Wipe",    fx_wipe },
    { "Breathe",       fx_breathe },
    { "Gradient",      fx_gradient },
    { "Solid",         fx_solid },
    { "Bouncing Balls",fx_balls },
    { "Snow",          fx_snow },
    { "Fireworks",     fx_fireworks },
    { "Wave",          fx_wave },
    { "Ripple",        fx_ripple },
    { "Scanner",       fx_scanner },
    { "Checkerboard",  fx_checker },
    { "Bars",          fx_bars },
    { "Clouds",        fx_clouds },
    { "Radar",         fx_radar },
    { "Rings",         fx_rings },
    { "Meteor Shower", fx_meteor },
    { "Aurora",        fx_aurora },
    { "Pride",         fx_pride },
    { "DNA Helix",     fx_dna },
    { "Pharmacy Cross",fx_pharmacy },
    { "Cross Pulse",   fx_cross_pulse },
    { "Cross Chase",   fx_cross_chase },
    { "Cross Build",   fx_cross_build },
    { "Cross Blink",   fx_cross_blink },
    // beat-synced (Mixxx bridge; free-running 120 BPM without it)
    { "Beat Flash",    fx_beat_flash,  1 },
    { "Beat Strobe",   fx_beat_strobe, 1 },
    { "Beat Colors",   fx_beat_colors, 1 },
    { "Beat Chase",    fx_beat_chase,  1 },
    { "Beat Ripple",   fx_beat_ripple, 1 },
    { "Beat Bars",     fx_beat_bars,   1 },
    { "Bar Wipe",      fx_bar_wipe,    1 },
    { "VU Meter",      fx_vu_meter,    1 },
    { "Ripple Rain",   fx_ripple_rain,   1 },
    { "Stereo Ripple", fx_stereo_ripple, 1 },
    { "Ripple Tunnel", fx_ripple_tunnel, 1 },
    { "Starburst",     fx_starburst,     1 },
    { "Kaleidoscope",  fx_kaleidoscope,  1 },
    { "Laser Fan",     fx_laser_fan,     1 },
    { "Beat Bounce",   fx_beat_bounce,   1 },
    { "Beat Scanner",  fx_beat_scanner,  1 },
    { "Beat Plasma",   fx_beat_plasma,   1 },
    { "Sixteenths",    fx_sixteenths,    1 },
    // 3D & shapes (effects_art.c)
    { "Retro Sun",       fx_retro_sun,    0, 1 },
    { "Platonic Solids", fx_solids,       0, 1 },
    { "Tesseract",       fx_tesseract,    0, 1 },
    { "Donuts",          fx_donuts,       0, 1 },
    { "Disco Ball",      fx_disco_ball,   0, 1 },
    { "Demo Tunnel",     fx_demo_tunnel,  0, 1 },
    { "Wave Mesh",       fx_wave_mesh,    0, 1 },
    { "Julia Morph",     fx_julia,        0, 1 },
    { "Metaballs",       fx_metaballs,    0, 1 },
    { "Spirograph",      fx_spirograph,   0, 1 },
    { "Hyperspace",      fx_hyperspace,   0, 1 },
    // horses & birthday parties (effects_theme.c)
    { "Galloping Horses", fx_horses,       0, 2 },
    { "Carousel",         fx_carousel,     0, 2 },
    { "Birthday Cake",    fx_cake,         0, 2 },
    { "Balloons",         fx_balloons,     0, 2 },
    { "Party Banner",     fx_party_banner, 0, 2 },
};
const int g_effect_count = sizeof(g_effects) / sizeof(g_effects[0]);

void fx_render(int idx, uint32_t st, uint32_t frame)
{
    if (idx < 0 || idx >= g_effect_count) idx = 0;
    int fps = render_fps_effective();
    if (fps < 1) fps = 1;
    s_k = g_set.paused ? 0.0f : REF_FPS / (float)fps * render_speed();
    g_effects[idx].fn(st, frame);
}

// ----------------------------------------------------------- text effects
static int text_h(void) { return FONT_H * g_set.font_scale; }
static int text_w(const char *s) { return font_text_width(s) * g_set.font_scale; }
static int centered_y(void) { return (CH - text_h()) / 2; }

static void bg_fill(void) { canvas_fill(BG()); }

static void tf_static(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    canvas_text((CW - text_w(s)) / 2, centered_y(), s, g_set.font_scale, FG());
}

// Moving text is drawn on whole pixels so the glyph edges stay sharp, and it
// moves with frame-locked motion (see motion_advance) so it doesn't judder.
// (canvas_text_sub() still exists for smooth-but-soft sub-pixel motion.)
static int scroll_pos(int span) { return (int)(s_mpos % span); }

static void tf_scroll_left(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    motion_advance(45.0f);
    int w = text_w(s) + 20;
    int x = CW - scroll_pos(w + CW);
    canvas_text(x, centered_y(), s, g_set.font_scale, FG());
}

static void tf_scroll_right(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    motion_advance(45.0f);
    int w = text_w(s) + 20;
    int x = -w + scroll_pos(w + CW);
    canvas_text(x, centered_y(), s, g_set.font_scale, FG());
}

static void tf_scroll_up(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    motion_advance(25.0f);
    int y = CH - scroll_pos(CH + text_h());
    canvas_text((CW - text_w(s)) / 2, y, s, g_set.font_scale, FG());
}

static void tf_scroll_down(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    motion_advance(25.0f);
    int y = -text_h() + scroll_pos(CH + text_h());
    canvas_text((CW - text_w(s)) / 2, y, s, g_set.font_scale, FG());
}

static void tf_blink(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    if ((st / 400) % 2 == 0)
        canvas_text((CW - text_w(s)) / 2, centered_y(), s, g_set.font_scale, FG());
}

static void tf_typewriter(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    int n = font_text_len(s);
    int total = n + 8;
    int show = (int)((st / 200) % (uint32_t)total);
    if (show > n) show = n;
    char buf[TEXT_MAX];
    const char *p = s;
    int bi = 0;
    for (int i = 0; i < show && *p; i++) {
        const char *q = p;
        utf8_next(&p);
        while (q < p && bi < (int)sizeof(buf) - 1) buf[bi++] = *q++;
    }
    buf[bi] = 0;
    int x = (CW - text_w(s)) / 2;
    canvas_text(x, centered_y(), buf, g_set.font_scale, FG());
    if ((st / 200) % 2 == 0 && show < n)
        canvas_rect(x + text_w(buf), centered_y(), g_set.font_scale, text_h(), FG(), true);
}

typedef struct { uint32_t st; } cbctx_t;

static rgb_t cb_rainbow(int idx, int px, int py, void *ctx)
{
    cbctx_t *c = ctx;
    return hsv((uint8_t)(px * 3 - c->st / 20), 255, 255);
}

static void tf_rainbow(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    cbctx_t ctx = { st };
    canvas_text_cb((CW - text_w(s)) / 2, centered_y(), s, g_set.font_scale,
                   cb_rainbow, &ctx);
}

static void tf_wave(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    int scale = g_set.font_scale;
    int x = (CW - text_w(s)) / 2;
    int i = 0;
    const char *p = s;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        int dy = (int)(sinf(st / 250.0f + i * 0.6f) * 4);
        canvas_char(x, centered_y() + dy, cp, scale, FG());
        x += FONT_ADVANCE * scale;
        i++;
    }
}

static void tf_fade(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    float v = (sinf(st / 500.0f) + 1.0f) * 0.5f;
    canvas_text((CW - text_w(s)) / 2, centered_y(), s, g_set.font_scale,
                rgb_scale(FG(), (uint8_t)(v * 255)));
}

static void tf_bounce(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    motion_advance(40.0f);
    int span = CW - text_w(s);
    int x = span < 1 ? span / 2 : pingpong(s_mpos, span + 1);
    canvas_text(x, centered_y(), s, g_set.font_scale, FG());
}

static void tf_flash(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    canvas_text((CW - text_w(s)) / 2, centered_y(), s, g_set.font_scale,
                hsv((uint8_t)(st / 60), 255, 255));
}

static void tf_marquee(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    motion_advance(45.0f);
    int w = text_w(s) + 20;
    int x = CW - scroll_pos(w + CW);
    canvas_text(x, centered_y(), s, g_set.font_scale, FG());
    // chasing border
    int o = (st / 90) % 4;
    rgb_t bc = hsv((uint8_t)(st / 25), 255, 255);
    for (int i = o; i < CW; i += 4) { canvas_px(i, 0, bc); canvas_px(CW - 1 - i, CH - 1, bc); }
    for (int i = o; i < CH; i += 4) { canvas_px(0, CH - 1 - i, bc); canvas_px(CW - 1, i, bc); }
}

static void tf_slide(const char *s, uint32_t st, uint32_t f)
{
    bg_fill();
    motion_advance(60.0f);
    int target = (CW - text_w(s)) / 2;
    int travel = CW - target;                    // slide in from the right edge
    if (travel < 1) travel = 1;
    int d = scroll_pos(travel + 120);            // then hold ~2 s at the centre
    int x = d < travel ? CW - d : target;
    canvas_text(x, centered_y(), s, g_set.font_scale, FG());
}

const textanim_t g_textanims[] = {
    { "Static",       tf_static },
    { "Scroll Left",  tf_scroll_left },
    { "Scroll Right", tf_scroll_right },
    { "Scroll Up",    tf_scroll_up },
    { "Scroll Down",  tf_scroll_down },
    { "Blink",        tf_blink },
    { "Typewriter",   tf_typewriter },
    { "Rainbow",      tf_rainbow },
    { "Wave",         tf_wave },
    { "Fade",         tf_fade },
    { "Bounce",       tf_bounce },
    { "Flash Colors", tf_flash },
    { "Marquee",      tf_marquee },
    { "Slide In",     tf_slide },
};
const int g_textanim_count = sizeof(g_textanims) / sizeof(g_textanims[0]);

void textfx_render(int idx, const char *s, uint32_t st, uint32_t frame)
{
    if (idx < 0 || idx >= g_textanim_count) idx = 0;
    g_textanims[idx].fn(s, st, frame);
}

// ------------------------------------------------------------- clock mode
void clock_render(uint32_t st, uint32_t frame)
{
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[16];
    int h = tmv.tm_hour;
    if (!g_set.clock24) { h = h % 12; if (h == 0) h = 12; }
    if (g_set.clock_seconds)
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, tmv.tm_min, tmv.tm_sec);
    else
        snprintf(buf, sizeof(buf), "%02d:%02d", h, tmv.tm_min);
    canvas_fill(BG());
    int scale = g_set.font_scale;
    while (scale > 1 && font_text_width(buf) * scale > CW - 8) scale--;
    canvas_text((CW - font_text_width(buf) * scale) / 2,
                (CH - FONT_H * scale) / 2, buf, scale, FG());
}
