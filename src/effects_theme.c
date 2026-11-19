// Themed effects: horses and birthday parties. Pixel-art sprites are drawn as
// text grids (one character per pixel) so they're easy to edit by eye.
#include "effects_theme.h"
#include "effects.h"
#include "canvas.h"
#include "font.h"
#include "beatsync.h"
#include <math.h>
#include <string.h>

#define PI_F 3.14159265f

static uint32_t hash32(uint32_t h)
{
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h;
}

static uint8_t u8c(float v) { return v <= 0 ? 0 : (v >= 1 ? 255 : (uint8_t)(v * 255.0f + 0.5f)); }

static rgb_t mix(rgb_t a, rgb_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return RGB((uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
               (uint8_t)(a.b + (b.b - a.b) * t));
}

static float beat_env(float k)
{
    double b = g_bf.beats;
    return expf(-k * (float)(b - floor(b)));
}

// ----------------------------------------------------------------- sprites
#define HORSE_W 32
#define HORSE_H 19
// 4 gallop frames, facing right. B body, M mane/tail, H hoof.
static const char *const HORSE[4][HORSE_H] = {
    {
        ".........................MM.....",
        "........................MBBB....",
        ".......................MBBBBB...",
        "......................MBBBBBBB..",
        ".....................MBBBBBBBBB.",
        "....................MBBBBB.BBBBB",
        "...................MBBBBB....BB.",
        "..................MBBBBB........",
        ".................MBBBBBB........",
        "MMMM..........BBBBBBBBBBB.......",
        "..MMMM.BBBBBBBBBBBBBBBBBBB......",
        ".....MBBBBBBBBBBBBBBBBBBBB......",
        "......BBBBBBBBBBBBBBBBBBBB......",
        ".......BBBBBBBBBBBBBBBBBB.......",
        "......BB.............BB.........",
        ".....BB...............BB........",
        "....BB.................BB.......",
        "...BB...................BB......",
        "...H.....................HH.....",
    },
    {
        ".........................MM.....",
        "........................MBBB....",
        ".......................MBBBBB...",
        "......................MBBBBBBB..",
        ".....................MBBBBBBBBB.",
        "....................MBBBBB.BBBBB",
        "...................MBBBBB....BB.",
        "..................MBBBBB........",
        ".................MBBBBBB........",
        "..............BBBBBBBBBBB.......",
        "....MM.BBBBBBBBBBBBBBBBBBB......",
        "..MMM.BBBBBBBBBBBBBBBBBBBB......",
        ".MM...BBBBBBBBBBBBBBBBBBBB......",
        "M......BBBBBBBBBBBBBBBBBB.......",
        ".......BB...........BBB.........",
        "........BB.........BB.BB........",
        ".........BB.......BB...BB.......",
        "..........BB.....BB.....B.......",
        "...........H.....H......H.......",
    },
    {
        ".........................MM.....",
        "........................MBBB....",
        ".......................MBBBBB...",
        "......................MBBBBBBB..",
        ".....................MBBBBBBBBB.",
        "....................MBBBBB.BBBBB",
        "...................MBBBBB....BB.",
        "..................MBBBBB........",
        ".................MBBBBBB........",
        "MMMM..........BBBBBBBBBBB.......",
        "..MMMM.BBBBBBBBBBBBBBBBBBB......",
        ".....MBBBBBBBBBBBBBBBBBBBB......",
        "......BBBBBBBBBBBBBBBBBBBB......",
        ".......BBBBBBBBBBBBBBBBBB.......",
        "........BB.BB.....BB.BB.........",
        "........B...B.....B...B.........",
        ".......B....B....B....B.........",
        ".......B...B.....B...B..........",
        ".......H...H.....H...H..........",
    },
    {
        ".........................MM.....",
        "........................MBBB....",
        ".......................MBBBBB...",
        "......................MBBBBBBB..",
        ".....................MBBBBBBBBB.",
        "....................MBBBBB.BBBBB",
        "...................MBBBBB....BB.",
        "..................MBBBBB........",
        ".................MBBBBBB........",
        "..............BBBBBBBBBBB.......",
        "....MM.BBBBBBBBBBBBBBBBBBB......",
        "..MMM.BBBBBBBBBBBBBBBBBBBB......",
        ".MM...BBBBBBBBBBBBBBBBBBBB......",
        "M......BBBBBBBBBBBBBBBBBB.......",
        ".......BB.BB.......BBB..........",
        "......BB...B.......B..BB........",
        ".....BB.....B......B....BB......",
        ".....B......B......B......BB....",
        ".....H......H......H.......H....",
    },
};

#define CAKE_W 46
#define CAKE_H 22
// F frosting, C sponge, D jam, P plate, W/R candle stripes.
static const char *const CAKE[CAKE_H] = {
    "..............................................",
    "......W.......W.......W.......W.......W.......",
    "......R.......R.......R.......R.......R.......",
    "......W.......W.......W.......W.......W.......",
    "......R.......R.......R.......R.......R.......",
    ".....FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF.....",
    "....FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF....",
    "....CFFCCFFFCCCFFCCCFCCCFFFCCCFCCFFFCCFFCC....",
    "....CFCCCCFCCCCFCCCCCCCCCFCCCCCCCCFCCCCFCC....",
    "....CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC....",
    "....DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD....",
    "....CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC....",
    "..FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF..",
    ".FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF.",
    ".CFFCCFFFCCCFFCCFFCCCFFFCCFFCCCFFCCFFFCCCFFCC.",
    ".CCFCCCFCCCCCFCCCFCCCCFCCCCFCCCCFCCCCFCCCCFCC.",
    ".CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC.",
    ".DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD.",
    ".CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC.",
    ".CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC.",
    "PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP",
    ".PPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP.",
};

typedef struct { rgb_t body, mane, hoof; } horse_col_t;

static void draw_horse(int frame, int x, int y, const horse_col_t *c)
{
    for (int r = 0; r < HORSE_H; r++) {
        const char *row = HORSE[frame & 3][r];
        for (int i = 0; i < HORSE_W; i++) {
            switch (row[i]) {
            case 'B': canvas_px(x + i, y + r, c->body); break;
            case 'M': canvas_px(x + i, y + r, c->mane); break;
            case 'H': canvas_px(x + i, y + r, c->hoof); break;
            default: break;
            }
        }
    }
}

// --------------------------------------------------------- Galloping Horses
// A herd galloping past a fence. The fence scrolls on the frame-locked
// cadence (smooth); each horse slowly overtakes the camera and the others.
// Their stride is one beat long, so with Mixxx connected they gallop in time.
void fx_horses(uint32_t st, uint32_t f)
{
    static const horse_col_t COLS[4] = {
        { {170, 85, 30},  {80, 35, 12},   {70, 70, 70} },     // chestnut
        { {235, 185, 85}, {255, 245, 215}, {90, 80, 70} },    // palomino
        { {215, 215, 222}, {130, 130, 145}, {80, 80, 90} },   // grey
        { {95, 105, 140}, {45, 50, 70},   {60, 60, 70} },     // blue roan
    };
    canvas_clear();
    float t = st / 1000.0f;
    int32_t scroll = fx_motion(40.0f);
    // ground and grass
    for (int x = 0; x < CW; x++) {
        canvas_px(x, CH - 1, RGB(40, 120, 30));
        if (((x + scroll) % 7) == 0) canvas_px(x, CH - 2, RGB(25, 80, 20));
    }
    // fence: two rails, posts every 32 px
    for (int x = 0; x < CW; x++) {
        canvas_px(x, 17, RGB(85, 52, 25));
        canvas_px(x, 22, RGB(85, 52, 25));
    }
    for (int px = -(scroll % 32); px < CW; px += 32)
        for (int y = 15; y < CH - 1; y++) {
            canvas_px(px, y, RGB(120, 75, 35));
            canvas_px(px + 1, y, RGB(95, 60, 28));
        }
    // horses
    const float span = CW + HORSE_W + 30;
    for (int k = 0; k < 4; k++) {
        float x = fmodf(k * span / 4 + t * (5.0f + 2.5f * k), span) - HORSE_W - 15;
        double ph = g_bf.beats + k * 0.37;            // one stride per beat
        int frame = (int)floor(ph * 4.0) & 3;
        int bob = (frame == 0 || frame == 3) ? -1 : 0;   // airborne strides ride higher
        int ix = (int)x, iy = CH - 1 - HORSE_H + bob;
        // dust kicked up behind the hind hooves
        float age = (float)(ph - floor(ph));
        for (int j = 0; j < 4; j++) {
            float dx = age * 14.0f + j * 2.0f, dy = age * 3.0f + (j & 1);
            canvas_add(ix + 4 - (int)dx, CH - 2 - (int)dy,
                       rgb_scale(RGB(150, 120, 80), u8c((1.0f - age) * 0.55f)));
        }
        draw_horse(frame, ix, iy, &COLS[k]);
    }
}

// ----------------------------------------------------------------- Carousel
// A merry-go-round: pastel horses on twisted golden poles bob up and down as
// the carousel turns, under a striped, scalloped canopy with chasing bulbs.
void fx_carousel(uint32_t st, uint32_t f)
{
    static const horse_col_t COLS[5] = {
        { {245, 245, 245}, {255, 90, 170}, {220, 180, 60} },  // white, pink mane
        { {255, 150, 195}, {170, 60, 220}, {220, 180, 60} },  // pink
        { {150, 205, 255}, {255, 200, 60}, {220, 180, 60} },  // sky blue
        { {245, 205, 95},  {230, 40, 50},  {255, 255, 255} }, // gold
        { {200, 170, 255}, {60, 200, 170}, {220, 180, 60} },  // lavender
    };
    const int SPACING = 50, RING = SPACING * 5;   // 5 horses around the ring
    canvas_clear();
    float t = st / 1000.0f;
    int32_t rot = fx_motion(18.0f);
    // canopy stripes + scallops, turning with the ring
    for (int x = 0; x < CW; x++) {
        int seg = (int)(((x - rot) % RING + RING) % RING);
        bool red = (seg / 10) & 1;
        rgb_t c = red ? RGB(200, 20, 45) : RGB(240, 228, 215);
        for (int y = 0; y < 3; y++) canvas_px(x, y, c);
        int sx = seg % 10;                            // scallop: round bottom per stripe
        if (sx >= 1 && sx <= 8) canvas_px(x, 3, c);
        if (sx >= 3 && sx <= 6) canvas_px(x, 4, c);
        // bulbs along the canopy edge, chasing
        if (seg % 5 == 2) {
            bool lit = ((seg / 5) + (int)(st / 140)) % 3 == 0;
            canvas_px(x, 1, lit ? RGB(255, 235, 150) : RGB(120, 90, 35));
        }
    }
    // poles and horses
    for (int k = 0; k < 5; k++) {
        int cx = (int)(((k * SPACING + rot) % RING + RING) % RING) - 25;
        if (cx < -HORSE_W || cx > CW + HORSE_W) continue;
        for (int y = 5; y < CH - 1; y++) {            // twisted brass pole
            bool stripe = ((y + (int)(st / 90) + k * 3) % 6) < 2;
            rgb_t pc = stripe ? RGB(255, 240, 180) : RGB(210, 160, 45);
            canvas_px(cx, y, pc);
            canvas_px(cx + 1, y, rgb_scale(pc, 170));
        }
        float bob = sinf(t * 2.2f + k * 1.3f) * 2.5f;
        draw_horse(0, cx - 15, (int)lroundf(8.0f + bob), &COLS[k]);
    }
    // turning platform
    for (int x = 0; x < CW; x++) {
        int seg = (int)(((x - rot) % RING + RING) % RING);
        canvas_px(x, CH - 1, (seg / 6) & 1 ? RGB(220, 170, 50) : RGB(170, 30, 40));
    }
}

// ----------------------------------------------------------- Birthday Cake
// A two-tier cake with flickering candles, and HAPPY BIRTHDAY in cycling
// colours beside it, with twinkling sparkles.
typedef struct { uint32_t st; int offset; } letter_ctx_t;

static rgb_t letter_rainbow(int idx, int px, int py, void *ctx)
{
    letter_ctx_t *c = ctx;
    return hsv((uint8_t)((idx + c->offset) * 28 + c->st / 12), 200, 255);
}

static void sparkle(int x, int y, float v, rgb_t c)
{
    canvas_add(x, y, rgb_scale(c, u8c(v)));
    rgb_t d = rgb_scale(c, u8c(v * 0.45f));
    canvas_add(x - 1, y, d); canvas_add(x + 1, y, d);
    canvas_add(x, y - 1, d); canvas_add(x, y + 1, d);
}

void fx_cake(uint32_t st, uint32_t f)
{
    canvas_clear();
    const int cx = 6, cy = CH - CAKE_H;
    for (int r = 0; r < CAKE_H; r++)
        for (int i = 0; i < CAKE_W; i++) {
            rgb_t c;
            switch (CAKE[r][i]) {
            case 'F': c = RGB(255, 165, 205); break;
            case 'C': c = RGB(240, 195, 130); break;
            case 'D': c = RGB(205, 30, 85); break;
            case 'P': c = RGB(170, 175, 200); break;
            case 'W': c = RGB(235, 235, 255); break;
            case 'R': c = RGB(255, 60, 60); break;
            default: continue;
            }
            canvas_px(cx + i, cy + r, c);
        }
    // candle flames: random height and sway every 70 ms
    static const int CANDLE_X[5] = { 6, 14, 22, 30, 38 };
    uint32_t slot = st / 70;
    for (int i = 0; i < 5; i++) {
        uint32_t h = hash32(slot * 31u + i);
        int x = cx + CANDLE_X[i] + ((h & 7) == 0 ? ((h >> 3) & 1 ? 1 : -1) : 0);
        int top = cy;                                  // just above the wick
        int tall = 2 + (int)(h >> 4 & 1);
        for (int dy = -2; dy <= 2; dy++)               // warm glow
            for (int dx = -2; dx <= 2; dx++) {
                float w = 1.0f - (dx * dx + dy * dy) / 8.0f;
                if (w > 0) canvas_add(x + dx, top - 1 + dy, rgb_scale(RGB(120, 50, 0), u8c(w * 0.6f)));
            }
        canvas_px(x, top, RGB(255, 245, 170));
        canvas_px(x, top - 1, RGB(255, 175, 40));
        if (tall > 2) canvas_px(x, top - 2, RGB(255, 90, 20));
    }
    // HAPPY / BIRTHDAY beside the cake
    const int area_x = cx + CAKE_W + 8, area_w = CW - area_x - 2;
    const char *l1 = "HAPPY", *l2 = "BIRTHDAY";
    int w1 = font_text_width(l1) * 2, w2 = font_text_width(l2) * 2;
    letter_ctx_t c1 = { st, 0 }, c2 = { st, 5 };
    canvas_text_cb(area_x + (area_w - w1) / 2, 0, l1, 2, letter_rainbow, &c1);
    canvas_text_cb(area_x + (area_w - w2) / 2, CH - FONT_H * 2, l2, 2, letter_rainbow, &c2);
    // sparkles twinkling around the text
    for (int i = 0; i < 9; i++) {
        uint32_t s = st / 280 + i * 7;
        uint32_t h = hash32(s * 2654435761u + i);
        float ph = (float)(st % 280) / 280.0f;
        float v = sinf(ph * PI_F);
        sparkle(area_x + (int)(h % area_w), (int)((h >> 12) % CH), v, RGB(255, 255, 220));
    }
}

// ----------------------------------------------------------------- Balloons
// Party balloons with highlights and wavy strings drift up and sway; every
// so often one pops into fragments.
void fx_balloons(uint32_t st, uint32_t f)
{
    static const rgb_t COL[8] = {
        {235, 30, 40}, {255, 200, 20}, {30, 90, 255}, {30, 200, 70},
        {255, 90, 180}, {150, 60, 230}, {255, 120, 20}, {20, 210, 220},
    };
    canvas_clear();
    float t = st / 1000.0f;
    const int N = 16;
    for (int i = 0; i < N; i++) {
        uint32_t hi = hash32(i * 977u + 5);
        float speed = 3.5f + (hi & 7) * 0.6f;          // px per second
        float cyc = t * speed / 50.0f + (hi >> 8 & 255) / 255.0f;
        int32_t n = (int32_t)floorf(cyc);
        float y = 37.0f - (cyc - n) * 50.0f;          // centre: below -> above the panel
        uint32_t hc = hash32((uint32_t)(i * 7919 + n * 104729));
        // each balloon keeps to its own slice of the width, so they spread out
        float slice = (float)(CW - 16) / N;
        float x = 8.0f + slice * i + (float)(hc % (uint32_t)slice) + sinf(t * (0.6f + 0.1f * (i % 3)) + i) * 3.0f;
        rgb_t c = COL[(hc >> 12) & 7];
        // a quarter of the trips end in a pop partway up
        float pop_y = 5.0f + (float)((hc >> 24) % 10);
        if ((hc >> 20) % 4 == 0 && y < pop_y) {
            float age = (pop_y - y) / speed;           // seconds since the pop
            if (age < 0.4f) {
                if (age < 0.06f) {
                    for (int dy = -2; dy <= 2; dy++)
                        for (int dx = -3; dx <= 3; dx++) canvas_add((int)x + dx, (int)pop_y + dy, RGB(110, 110, 110));
                }
                for (int j = 0; j < 10; j++) {
                    float a = j * 0.628f + (hc & 7) * 0.2f, r = 4.0f + age * 40.0f;
                    canvas_add((int)(x + cosf(a) * r), (int)(pop_y + sinf(a) * r * 0.5f + age * age * 30.0f),
                               rgb_scale(c, u8c(1.0f - age / 0.4f)));
                }
            }
            continue;
        }
        int ix = (int)lroundf(x), iy = (int)lroundf(y);
        for (int j = 0; j < 9; j++) {                 // string
            int sx = ix + (int)lroundf(sinf(t * 3.0f + j * 0.6f + i) * 1.0f);
            canvas_px(sx, iy + 6 + j, RGB(110, 110, 120));
        }
        for (int dy = -6; dy <= 5; dy++)              // balloon body, shaded
            for (int dx = -5; dx <= 5; dx++) {
                float ex = dx / 4.4f, ey = dy / 5.6f;
                if (ex * ex + ey * ey > 1.0f) continue;
                float shade = 1.0f - 0.35f * (dx + dy + 6) / 12.0f;
                canvas_px(ix + dx, iy + dy, rgb_scale(c, u8c(shade)));
            }
        canvas_px(ix - 2, iy - 4, RGB(255, 255, 255));  // highlight
        canvas_px(ix - 1, iy - 4, RGB(200, 200, 200));
        canvas_px(ix - 2, iy - 3, RGB(200, 200, 200));
        canvas_px(ix, iy + 6, rgb_scale(c, 150));      // knot
    }
}

// ------------------------------------------------------------ Party Banner
// A fluttering bunting garland across the top, HAPPY BIRTHDAY in gold with a
// shine sweeping across, and confetti tumbling down behind it.
typedef struct { uint32_t st; float glint_x, pulse; } gold_ctx_t;

static rgb_t gold_shine(int idx, int px, int py, void *ctx)
{
    gold_ctx_t *g = ctx;
    float d = fabsf(px - g->glint_x);
    rgb_t c = mix(RGB(255, 205, 40), RGB(255, 255, 235), 1.0f - d / 7.0f);
    return rgb_scale(c, u8c(g->pulse));
}

static float swag_y(float x)
{
    // two swags hung from x = 0, 100 and 199
    float x0 = x < 100 ? 0 : 100, x1 = x < 100 ? 100 : CW - 1;
    return 0.5f + 4.5f * sinf(PI_F * (x - x0) / (x1 - x0));
}

void fx_party_banner(uint32_t st, uint32_t f)
{
    static const rgb_t FLAG[6] = {
        {235, 30, 50}, {255, 200, 20}, {30, 190, 70}, {40, 110, 255}, {255, 90, 180}, {255, 120, 20},
    };
    canvas_clear();
    float t = st / 1000.0f;
    // confetti, behind everything
    for (int i = 0; i < 45; i++) {
        uint32_t h = hash32(i * 2654435761u + 3);
        float v = 5.0f + (h & 15) * 0.5f;
        float cyc = t * v / 36.0f + (h >> 8 & 255) / 255.0f;
        int32_t n = (int32_t)floorf(cyc);
        uint32_t hc = hash32((uint32_t)(i * 977 + n * 7919));
        float y = -3.0f + (cyc - n) * 36.0f;
        float x = (float)(hc % CW) + sinf(t * 2.0f + i) * 3.0f;
        rgb_t c = hsv((uint8_t)(hc >> 8), 220, 230);
        int ix = (int)x, iy = (int)y;
        canvas_px(ix, iy, c);
        if (((int)(t * 8.0f) + i) & 1) canvas_px(ix + 1, iy, rgb_scale(c, 160));   // tumbling
    }
    // bunting string and flags
    for (int x = 0; x < CW; x++) canvas_px(x, (int)lroundf(swag_y(x)), RGB(150, 150, 150));
    int k = 0;
    for (int fx = 5; fx < CW - 3; fx += 11, k++) {
        float top = swag_y(fx) + 1.0f;
        float sway = sinf(t * 3.0f + k * 0.7f) * 1.3f;
        for (int r = 0; r < 6; r++) {
            float half = 3.2f * (1.0f - r / 6.0f);
            float c = fx + sway * r / 5.0f;
            for (int x = (int)lroundf(c - half); x <= (int)lroundf(c + half); x++)
                canvas_px(x, (int)top + r, FLAG[k % 6]);
        }
    }
    // HAPPY BIRTHDAY with a shine sweeping across, pulsing on the beat
    const char *msg = "HAPPY BIRTHDAY";
    int w = font_text_width(msg) * 2;
    gold_ctx_t g = { st, fmodf(t * 70.0f, CW + 80.0f) - 40.0f, 0.85f + 0.15f * beat_env(4.0f) };
    canvas_text_cb((CW - w) / 2, CH - FONT_H * 2 - 1, msg, 2, gold_shine, &g);
}
