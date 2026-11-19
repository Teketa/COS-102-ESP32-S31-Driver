// Artistic effects: shapes and 3D on a 200x30 panel.
//
// Lines are anti-aliased (Wu-style, coverage square-rooted to roughly undo the
// panel's gamma) so slowly rotating wireframes glide instead of crawling pixel
// by pixel, and additive so crossing edges and vertices glow. Everything moves
// on the speed-scaled clock `st`; effects that simulate per frame use
// fx_frame_k() so speed doesn't depend on the frame rate.
#include "effects_art.h"
#include "effects.h"
#include "canvas.h"
#include "beatsync.h"
#include <math.h>
#include <string.h>

#define PI_F  3.14159265f
#define MINF(a, b) ((a) < (b) ? (a) : (b))
#define MAXF(a, b) ((a) > (b) ? (a) : (b))
#define TAU_F 6.28318531f

// ------------------------------------------------------------ 3D helpers
typedef struct { float x, y, z; } v3;
typedef struct { float m[3][3]; } m3;

static m3 rot_xyz(float ax, float ay, float az)
{
    float cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay),
          cz = cosf(az), sz = sinf(az);
    m3 r = {{
        { cy * cz, cz * sx * sy - cx * sz, cx * cz * sy + sx * sz },
        { cy * sz, cx * cz + sx * sy * sz, cx * sy * sz - cz * sx },
        { -sy,     cy * sx,                cx * cy },
    }};
    return r;
}

static v3 mul(const m3 *r, v3 p)
{
    return (v3){ r->m[0][0] * p.x + r->m[0][1] * p.y + r->m[0][2] * p.z,
                 r->m[1][0] * p.x + r->m[1][1] * p.y + r->m[1][2] * p.z,
                 r->m[2][0] * p.x + r->m[2][1] * p.y + r->m[2][2] * p.z };
}

// Perspective: camera `dist` units in front of the origin, `scale` px per unit
// at the origin.
static void project(v3 p, float cx, float cy, float scale, float dist, float *sx, float *sy)
{
    float k = scale * dist / (p.z + dist);
    *sx = cx + p.x * k;
    *sy = cy + p.y * k;
}

// Coverage -> brightness, square-rooted to roughly undo the panel gamma.
static uint8_t s_cov[65];
static bool s_cov_ok;

static inline void plot_w(int x, int y, rgb_t c, float w)
{
    if ((unsigned)x >= CW || (unsigned)y >= CH || w <= 0.01f) return;
    int i = (int)(w * 64.0f);
    canvas_add(x, y, rgb_scale(c, s_cov[i > 64 ? 64 : i]));
}

// Anti-aliased line between sub-pixel endpoints, added to the canvas.
static void line_aa(float x0, float y0, float x1, float y1, rgb_t c)
{
    if (!s_cov_ok) {
        for (int i = 0; i <= 64; i++) s_cov[i] = (uint8_t)(255.0f * sqrtf(i / 64.0f));
        s_cov_ok = true;
    }
    // skip lines entirely off the panel
    if ((x0 < -1 && x1 < -1) || (x0 > CW && x1 > CW) || (y0 < -1 && y1 < -1) || (y0 > CH && y1 > CH))
        return;
    bool steep = fabsf(y1 - y0) > fabsf(x1 - x0);
    float t;
    if (steep) { t = x0; x0 = y0; y0 = t; t = x1; x1 = y1; y1 = t; }
    if (x0 > x1) { t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; }
    float dx = x1 - x0, g = dx < 1e-4f ? 0.0f : (y1 - y0) / dx;
    int xs = (int)floorf(x0 + 0.5f), xe = (int)floorf(x1 + 0.5f);
    int lim = steep ? CH : CW;                   // clip the long axis early
    if (xs < -1) xs = -1;
    if (xe > lim) xe = lim;
    float y = y0 + g * ((float)xs - x0);
    for (int x = xs; x <= xe; x++, y += g) {
        int yi = (int)floorf(y);
        float fr = y - (float)yi;
        if (steep) { plot_w(yi, x, c, 1.0f - fr); plot_w(yi + 1, x, c, fr); }
        else       { plot_w(x, yi, c, 1.0f - fr); plot_w(x, yi + 1, c, fr); }
    }
}

static uint32_t hash32(uint32_t h)
{
    h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
    return h;
}

static uint8_t u8c(float v) { return v <= 0 ? 0 : (v >= 1 ? 255 : (uint8_t)(v * 255.0f + 0.5f)); }

static rgb_t mix(rgb_t a, rgb_t b, uint8_t t)
{
    return RGB((uint8_t)(a.r + (b.r - a.r) * t / 255), (uint8_t)(a.g + (b.g - a.g) * t / 255),
               (uint8_t)(a.b + (b.b - a.b) * t / 255));
}

static float beat_env(float k)
{
    double b = g_bf.beats;
    return expf(-k * (float)(b - floor(b)));
}

// ---------------------------------------------------------------- state
static struct {
    float pen[4];                                // spirograph pen angles
    struct { float x, y, z; } star[90];          // hyperspace
    bool stars_ok;
} A;

void art_reset(void) { memset(&A, 0, sizeof(A)); }

// ------------------------------------------------------------ Retro Sun
// Synthwave sunset: striped sun, mountains, and a neon grid rushing toward
// the viewer; the grid glows brighter on the beat.
void fx_retro_sun(uint32_t st, uint32_t f)
{
    const int HZ = 13;                               // horizon row
    const float scx = CW / 2.0f, scy = HZ - 1.0f, sr = 12.5f;
    float scroll = st / 700.0f;                      // grid motion
    float glow = 0.55f + 0.45f * beat_env(3.0f);
    for (int y = 0; y < HZ; y++) {                   // sky gradient
        float t = (float)y / HZ;
        rgb_t sky = RGB((uint8_t)(20 + 110 * t * t), 0, (uint8_t)(45 + 50 * t));
        for (int x = 0; x < CW; x++) canvas_px(x, y, sky);
    }
    // sun with the classic bands cut out of its lower half (bands drift down)
    for (int y = 0; y < HZ; y++)
        for (int x = (int)(scx - sr - 1); x <= (int)(scx + sr + 1); x++) {
            float dx = x - scx, dy = (y - scy) * 1.0f;
            if (dx * dx + dy * dy > sr * sr) continue;
            float v = (y - (scy - sr)) / (2 * sr);   // 0 top .. ~0.5 at the horizon
            if (v > 0.18f) {
                float band = fmodf(y + st / 400.0f, 3.0f);
                if (band < (v - 0.18f) * 7.0f) continue;     // gaps widen downward
            }
            canvas_px(x, y, RGB(255, (uint8_t)(220 - 180 * v), (uint8_t)(60 + 70 * v)));
        }
    // mountains on both sides of the sun
    for (int x = 0; x < CW; x++) {
        float d = fabsf(x - scx);
        if (d < sr + 6) continue;
        float h = 2.5f + 2.2f * fabsf(sinf(x * 0.071f)) + 1.6f * sinf(x * 0.23f + 1.3f);
        h *= MINF(1.0f, (d - sr - 6) / 12.0f);
        for (int y = HZ - (int)h; y < HZ; y++) canvas_px(x, y, RGB(40, 8, 70));
    }
    // floor grid
    for (int y = HZ; y < CH; y++) {
        float d0 = y - HZ + 0.02f, d1 = y - HZ + 1.0f;
        float z0 = 15.0f / d0, z1 = 15.0f / d1;       // depth at the row's top/bottom edge
        bool hline = floorf(z0 * 0.5f + scroll) != floorf(z1 * 0.5f + scroll) || y == HZ;
        float zc = 15.0f / (y - HZ + 0.5f);
        float fog = MINF(1.0f, 4.0f / zc);
        float pxw = zc / 22.0f;                        // world width of one pixel
        for (int x = 0; x < CW; x++) {
            float wx = (x - scx) * pxw;
            float g = fabsf(wx / 2.5f - roundf(wx / 2.5f)) * 2.5f;   // distance to a line
            float v = MAXF(0.0f, 1.0f - g / MAXF(pxw, 0.3f));
            if (hline) v = 1.0f;
            rgb_t base = RGB(10, 0, 26);
            if (v > 0) {
                rgb_t line = y == HZ ? RGB(255, 80, 200) : RGB(255, 40, 190);
                base = mix(base, line, u8c(v * fog * glow));
            }
            canvas_px(x, y, base);
        }
    }
}

// ------------------------------------------------------- Platonic Solids
#define PHI_F 1.6180339887f
static v3 s_sv[64];
static uint8_t s_se[96][2];
static struct { uint8_t v0, nv, e0, ne; } s_solid[5];
static bool s_solids_ok;

static void add_solid(int idx, const v3 *v, int nv, int *vtot, int *etot)
{
    // normalise to unit circumradius, then connect every pair at the
    // shortest vertex distance -- that's exactly the edge set of a Platonic solid
    float r = sqrtf(v[0].x * v[0].x + v[0].y * v[0].y + v[0].z * v[0].z);
    int v0 = *vtot, e0 = *etot;
    for (int i = 0; i < nv; i++) s_sv[v0 + i] = (v3){ v[i].x / r, v[i].y / r, v[i].z / r };
    float dmin = 1e9f;
    for (int i = 0; i < nv; i++)
        for (int j = i + 1; j < nv; j++) {
            v3 a = s_sv[v0 + i], b = s_sv[v0 + j];
            float d = sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
            if (d < dmin) dmin = d;
        }
    for (int i = 0; i < nv; i++)
        for (int j = i + 1; j < nv; j++) {
            v3 a = s_sv[v0 + i], b = s_sv[v0 + j];
            float d = sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
            if (fabsf(d - dmin) < dmin * 0.01f && *etot < 96) {
                s_se[*etot][0] = (uint8_t)(v0 + i);
                s_se[*etot][1] = (uint8_t)(v0 + j);
                (*etot)++;
            }
        }
    s_solid[idx].v0 = (uint8_t)v0; s_solid[idx].nv = (uint8_t)nv;
    s_solid[idx].e0 = (uint8_t)e0; s_solid[idx].ne = (uint8_t)(*etot - e0);
    *vtot += nv;
}

static void solids_init(void)
{
    if (s_solids_ok) return;
    int vt = 0, et = 0, n;
    v3 v[20];
    const float p = PHI_F, q = 1.0f / PHI_F;
    v3 tet[4] = { {1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1} };
    add_solid(0, tet, 4, &vt, &et);
    n = 0;
    for (int i = 0; i < 8; i++) v[n++] = (v3){ i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1 };
    add_solid(1, v, n, &vt, &et);
    v3 oct[6] = { {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1} };
    add_solid(2, oct, 6, &vt, &et);
    n = 0;
    for (int i = 0; i < 4; i++) {
        float a = i & 1 ? 1 : -1, b = i & 2 ? p : -p;
        v[n++] = (v3){ 0, a, b }; v[n++] = (v3){ a, b, 0 }; v[n++] = (v3){ b, 0, a };
    }
    add_solid(3, v, n, &vt, &et);
    n = 0;
    for (int i = 0; i < 8; i++) v[n++] = (v3){ i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? 1 : -1 };
    for (int i = 0; i < 4; i++) {
        float a = i & 1 ? q : -q, b = i & 2 ? p : -p;
        v[n++] = (v3){ 0, a, b }; v[n++] = (v3){ a, b, 0 }; v[n++] = (v3){ b, 0, a };
    }
    add_solid(4, v, n, &vt, &et);
    s_solids_ok = true;
}

// All five Platonic solids side by side, tumbling at different rates.
void fx_solids(uint32_t st, uint32_t f)
{
    solids_init();
    canvas_clear();
    float t = st / 1000.0f;
    for (int s = 0; s < 5; s++) {
        m3 r = rot_xyz(t * (0.55f + 0.11f * s), t * (0.83f - 0.07f * s), t * 0.31f * (s & 1 ? 1 : -1));
        float cx = (2 * s + 1) * CW / 10.0f, cy = (CH - 1) / 2.0f;
        uint8_t hue = (uint8_t)(s * 51 + (int)(t * 12));
        v3 pv[20];
        float px[20], py[20];
        for (int i = 0; i < s_solid[s].nv; i++) {
            pv[i] = mul(&r, s_sv[s_solid[s].v0 + i]);
            project(pv[i], cx, cy, 10.5f, 3.5f, &px[i], &py[i]);
        }
        for (int e = 0; e < s_solid[s].ne; e++) {
            int a = s_se[s_solid[s].e0 + e][0] - s_solid[s].v0;
            int b = s_se[s_solid[s].e0 + e][1] - s_solid[s].v0;
            float z = (pv[a].z + pv[b].z) * 0.5f;            // -1 near .. 1 far
            rgb_t c = hsv(hue, 210, u8c(0.95f - 0.4f * z));
            line_aa(px[a], py[a], px[b], py[b], c);
        }
    }
}

// --------------------------------------------------------------- Tesseract
// A 4D hypercube: 16 vertices at (+-1,+-1,+-1,+-1), edges between vertices
// that differ in one coordinate. Rotated in two 4D planes (XW, ZW), then
// projected 4D -> 3D -> panel. Edges are coloured by the axis they run along.
void fx_tesseract(uint32_t st, uint32_t f)
{
    canvas_clear();
    float t = st / 1000.0f;
    static const uint8_t axis_hue[4] = { 128, 213, 40, 170 };
    for (int k = 0; k < 3; k++) {
        float ph = k * 2.1f;
        float a = t * 0.7f + ph, b = t * 0.45f + ph * 0.5f;
        float ca = cosf(a), sa = sinf(a), cb = cosf(b), sb = sinf(b);
        m3 r = rot_xyz(0.5f + t * 0.21f, t * 0.33f + ph, 0.2f);
        float px[16], py[16], pz[16];
        for (int i = 0; i < 16; i++) {
            float x = i & 1 ? 1 : -1, y = i & 2 ? 1 : -1, z = i & 4 ? 1 : -1, w = i & 8 ? 1 : -1;
            float x2 = x * ca - w * sa, w2 = x * sa + w * ca;     // XW plane
            float z2 = z * cb - w2 * sb, w3 = z * sb + w2 * cb;   // ZW plane
            float k4 = 1.0f / (2.6f - w3);                        // 4D -> 3D perspective
            v3 p = mul(&r, (v3){ x2 * k4 * 1.6f, y * k4 * 1.6f, z2 * k4 * 1.6f });
            pz[i] = p.z;
            project(p, (2 * k + 1) * CW / 6.0f, (CH - 1) / 2.0f, 8.0f, 3.0f, &px[i], &py[i]);
        }
        for (int i = 0; i < 16; i++)
            for (int ax = 0; ax < 4; ax++) {
                int j = i | (1 << ax);
                if (j == i) continue;
                float z = (pz[i] + pz[j]) * 0.5f;
                rgb_t c = hsv((uint8_t)(axis_hue[ax] + k * 20 + (int)(t * 8)), 200, u8c(0.9f - 0.35f * z));
                line_aa(px[i], py[i], px[j], py[j], c);
            }
    }
}

// ------------------------------------------------------------------ Donuts
// Shaded tori (the classic "spinning donut"): surface points are projected
// with a 1/z depth buffer and lit by a fixed light, so each donut shows real
// shading as it tumbles.
#define DT 32     // samples around the tube
#define DP 100    // samples around the ring (each splatted 2x2, so no holes)
static float s_ct[DT], s_stt[DT], s_cp[DP], s_sp[DP];
static bool s_donut_ok;
static uint8_t s_zb[CH][CW];

static void donut(float cx, float A_, float B_, uint8_t hue)
{
    const float R1 = 1.0f, R2 = 2.0f, K2 = 5.0f, K1 = 23.0f;
    float cA = cosf(A_), sA = sinf(A_), cB = cosf(B_), sB = sinf(B_);
    for (int i = 0; i < DT; i++) {
        float ct = s_ct[i], stt = s_stt[i];
        float ox = R2 + R1 * ct, oy = R1 * stt;
        for (int j = 0; j < DP; j++) {
            float cp = s_cp[j], sp = s_sp[j];
            float x = ox * (cB * cp + sA * sB * sp) - oy * cA * sB;
            float y = ox * (sB * cp - sA * cB * sp) + oy * cA * cB;
            float z = K2 + cA * ox * sp + oy * sA;
            float ooz = 1.0f / z;
            int xp = (int)(cx + K1 * ooz * x), yp = (int)((CH - 1) / 2.0f - K1 * ooz * y * 0.95f);
            uint8_t zq = (uint8_t)(ooz * 500.0f);
            // hidden behind nearer surface on all four pixels? skip the shading
            if ((unsigned)xp < CW - 1 && (unsigned)yp < CH - 1 &&
                zq <= s_zb[yp][xp] && zq <= s_zb[yp][xp + 1] &&
                zq <= s_zb[yp + 1][xp] && zq <= s_zb[yp + 1][xp + 1])
                continue;
            float L = cp * ct * sB - cA * ct * sp - sA * stt + cB * (cA * stt - ct * sA * sp);
            float v = L > 0 ? 0.12f + 0.88f * L / 1.4142f : 0.08f;
            rgb_t col = hsv((uint8_t)(hue + (int)(L * 18.0f)), 225, u8c(v));
            for (int sy = 0; sy < 2; sy++)
                for (int sx = 0; sx < 2; sx++) {
                    int px = xp + sx, py = yp + sy;
                    if ((unsigned)px >= CW || (unsigned)py >= CH) continue;
                    // the sample's own pixel wins ties over its splat neighbours
                    uint8_t zz = (sx | sy) ? (uint8_t)(zq - 1) : zq;
                    if (zz <= s_zb[py][px]) continue;
                    s_zb[py][px] = zz;
                    canvas_px(px, py, col);
                }
        }
    }
}

void fx_donuts(uint32_t st, uint32_t f)
{
    if (!s_donut_ok) {
        for (int i = 0; i < DT; i++) { s_ct[i] = cosf(i * TAU_F / DT); s_stt[i] = sinf(i * TAU_F / DT); }
        for (int j = 0; j < DP; j++) { s_cp[j] = cosf(j * TAU_F / DP); s_sp[j] = sinf(j * TAU_F / DP); }
        s_donut_ok = true;
    }
    canvas_clear();
    memset(s_zb, 0, sizeof(s_zb));
    float t = st / 1000.0f;
    for (int k = 0; k < 3; k++)
        donut((2 * k + 1) * CW / 6.0f, t * (0.9f + 0.17f * k) + k, t * (0.55f - 0.1f * k) + 2 * k,
              (uint8_t)(k * 85 + (int)(t * 10)));
    // Fill pinholes where the surface is stretched edge-on: a pixel that the
    // near surface missed is either empty or shows the dim far side through.
    // Either way it's much deeper than the pixels on both sides of it.
    const int GAP = 10;                              // in 1/z buffer units
    for (int y = 1; y < CH - 1; y++)
        for (int x = 1; x < CW - 1; x++) {
            int z = s_zb[y][x];
            if (s_zb[y][x - 1] > z + GAP && s_zb[y][x + 1] > z + GAP) {
                g_canvas[y][x] = g_canvas[y][x - 1];
                s_zb[y][x] = s_zb[y][x - 1];
            } else if (s_zb[y - 1][x] > z + GAP && s_zb[y + 1][x] > z + GAP) {
                g_canvas[y][x] = g_canvas[y - 1][x];
                s_zb[y][x] = s_zb[y - 1][x];
            }
        }
}

// --------------------------------------------------------------- Disco Ball
// A mirror ball of tiles turning in the middle, throwing light spots that
// sweep across the whole sign. Random tiles glint, more of them on the beat.
// The sphere's per-pixel latitude/longitude never change (only the rotation
// does), so they're computed once.
#define BALL_R 14.2f
#define BALL_N 31                                    // bounding box side, px
static struct { int16_t lat; uint16_t lon; uint8_t nz; } s_ball[BALL_N][BALL_N];
static bool s_ball_ok;

static void ball_init(void)
{
    const float bx = (CW - 1) / 2.0f, by = (CH - 1) / 2.0f;
    for (int j = 0; j < BALL_N; j++)
        for (int i = 0; i < BALL_N; i++) {
            int x = (int)(bx - BALL_N / 2) + i, y = (int)(by - BALL_N / 2) + j;
            float nx = (x - bx) / BALL_R, ny = (y - by) / BALL_R, rr = nx * nx + ny * ny;
            if (rr > 1.0f) { s_ball[j][i].lat = -1; continue; }
            float nz = sqrtf(1.0f - rr);
            s_ball[j][i].lat = (int16_t)((asinf(ny) / PI_F + 0.5f) * 256.0f);      // 0..256
            s_ball[j][i].lon = (uint16_t)((atan2f(nx, nz) / TAU_F + 0.5f) * 65536.0f);
            s_ball[j][i].nz = (uint8_t)(nz * 255.0f);
        }
    s_ball_ok = true;
}

void fx_disco_ball(uint32_t st, uint32_t f)
{
    const int LAT = 12, LON = 26, SPOTS = 46;
    const float bx = (CW - 1) / 2.0f, by = (CH - 1) / 2.0f;
    if (!s_ball_ok) ball_init();
    float rot = st / 2600.0f;                        // ball turns (in turns)
    float beat = beat_env(5.0f);
    uint32_t slot = st / 90;                         // glint timing
    canvas_clear();
    // reflections on the "walls": spots move with the ball's rotation
    for (int i = 0; i < SPOTS; i++) {
        uint32_t h = hash32(i * 7919u + 1);
        float lon = (h & 1023) / 1024.0f;
        float x = fmodf((lon + rot * 0.9f) * (CW + 60), (float)(CW + 60)) - 30.0f;
        float y = 2.0f + (float)((h >> 10) % (CH - 4));
        float tw = 0.55f + 0.45f * (hash32(h ^ slot) & 255) / 255.0f;
        rgb_t c = hsv((uint8_t)(h >> 20), (uint8_t)(40 + (h >> 28) * 6), u8c(tw * (0.6f + 0.4f * beat)));
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -2; dx <= 2; dx++) {
                float w = 1.0f - (dx * dx / 6.0f + dy * dy / 2.5f);
                if (w > 0) canvas_add((int)x + dx, (int)y + dy, rgb_scale(c, (uint8_t)(255 * w)));
            }
    }
    // the ball: tile grid on the sphere, grout lines, facing-dependent shade
    uint32_t rot16 = (uint32_t)(int64_t)(rot * 65536.0f);
    int glint_mod = beat > 0.6f ? 7 : 31;
    for (int j = 0; j < BALL_N; j++)
        for (int i = 0; i < BALL_N; i++) {
            if (s_ball[j][i].lat < 0) continue;
            int x = (int)(bx - BALL_N / 2) + i, y = (int)(by - BALL_N / 2) + j;
            int fl = s_ball[j][i].lat * LAT;                          // /256 = tile row
            uint32_t fo = ((s_ball[j][i].lon + rot16) & 0xFFFF) * LON;  // /65536 = tile column
            int ti = fl >> 8, to = (int)(fo >> 16);
            bool grout = (fl & 255) < 31 || (fo & 0xFFFF) < 7864;      // ~12 % of a tile
            uint32_t th = hash32((uint32_t)(ti * 131 + to));
            float nz = s_ball[j][i].nz / 255.0f;
            float shade = 0.18f + 0.55f * nz + 0.25f * (th & 255) / 255.0f;
            bool glint = (hash32(th ^ slot) % glint_mod) == 0 && nz > 0.25f;
            rgb_t c;
            if (grout) c = RGB(10, 10, 14);
            else if (glint) c = RGB(255, 255, 255);
            else c = hsv((uint8_t)(150 + (th >> 8 & 31)), 50, u8c(shade));
            canvas_px(x, y, c);
        }
}

// ------------------------------------------------------------- Demo Tunnel
// The 90s demoscene tunnel: every pixel maps to (depth, angle) on a textured
// tube, which scrolls forward and twists. Darker in the distance. Uses the
// shared radial tables plus a depth lookup, so it's cheap.
void fx_demo_tunnel(uint32_t st, uint32_t f)
{
    static uint16_t depth8[256];                     // 8.8 fixed point depth per radius
    static bool ok;
    if (!ok) {
        for (int r = 0; r < 256; r++) depth8[r] = (uint16_t)(120.0f / (r + 0.6f) * 256.0f > 65535 ? 65535 : 120.0f / (r + 0.6f) * 256.0f);
        ok = true;
    }
    const uint8_t *rad = fx_radial_rad(), *ang = fx_radial_ang();
    float t = st / 1000.0f;
    uint32_t fwd = (uint32_t)(t * 3.2f * 256.0f);    // forward motion, 8.8
    uint8_t twist = (uint8_t)(int)(t * 0.35f * 256.0f);
    uint8_t hue0 = (uint8_t)(int)(t * 20.0f);
    rgb_t *out = (rgb_t *)g_canvas;
    for (int i = 0; i < CW * CH; i++) {
        int r = rad[i];
        uint32_t d = depth8[r] + fwd;
        int u = (int)(d >> 7);                       // 2 rings per depth unit
        int v = (uint8_t)(ang[i] + twist) >> 4;      // 16 segments around
        bool check = (u + v) & 1;
        float fog = r >= 55 ? 1.0f : r / 55.0f;
        out[i] = hsv((uint8_t)(u * 9 + hue0), check ? 230 : 160, u8c(fog * (check ? 1.0f : 0.25f)));
    }
}

// --------------------------------------------------------------- Wave Mesh
// Flying low over a wireframe ocean.
void fx_wave_mesh(uint32_t st, uint32_t f)
{
    enum { NX = 29, NZ = 10 };
    static float sx[NZ][NX], sy[NZ][NX], dz[NZ];
    float t = st / 1000.0f;
    float scroll = t * 1.4f, frac = scroll - floorf(scroll);
    const float camH = 1.7f, F = 24.0f, HZ = 3.5f;
    canvas_clear();
    for (int k = 0; k < NZ; k++) {
        float z = 1.2f + k - frac;                       // rows slide toward us
        float zw = z + floorf(scroll);                   // world z (keeps waves still)
        dz[k] = z;
        for (int i = 0; i < NX; i++) {
            float x = (i - NX / 2) * 1.0f;
            float h = 0.55f * sinf(0.45f * x + 0.55f * zw + t * 1.3f)
                    + 0.35f * sinf(0.9f * zw - 0.3f * x - t * 0.8f);
            sx[k][i] = CW / 2.0f + x * F * 2.2f / z;
            sy[k][i] = HZ + (camH - h) * F / z;
        }
    }
    for (int k = NZ - 1; k >= 0; k--) {                  // far to near
        float fog = MINF(1.0f, 2.4f / dz[k]);
        fog *= fog;                                      // far rows fade out fast
        rgb_t c = hsv((uint8_t)(170 + (int)(30 * fog)), 220, u8c(0.06f + 0.94f * fog));
        for (int i = 0; i < NX - 1; i++) line_aa(sx[k][i], sy[k][i], sx[k][i + 1], sy[k][i + 1], c);
        if (k < NZ - 1)
            for (int i = 0; i < NX; i++) line_aa(sx[k][i], sy[k][i], sx[k + 1][i], sy[k + 1][i], rgb_scale(c, 150));
    }
}

// ------------------------------------------------------------------- Julia
// Julia set fractal; its constant travels around a circle, so the shape
// morphs continuously. Point-symmetric, so half is computed and mirrored.
void fx_julia(uint32_t st, uint32_t f)
{
    const int MAXI = 28;
    float t = st / 1000.0f;
    // c on (just around) the boundary of the Mandelbrot set's main cardioid,
    // where Julia sets are most detailed: c = e^ia/2 - e^2ia/4, nudged in/out
    float a = t * 0.13f, k = 1.0f + 0.03f * sinf(t * 0.37f);
    float cr = k * (0.5f * cosf(a) - 0.25f * cosf(2 * a));
    float ci = k * (0.5f * sinf(a) - 0.25f * sinf(2 * a));
    float zoom = 1.0f + 0.25f * sinf(t * 0.17f);
    float s = 3.4f / CW / zoom;
    uint8_t hue0 = (uint8_t)(int)(t * 15.0f);
    for (int y = 0; y < CH / 2; y++)
        for (int x = 0; x < CW; x++) {
            float zr = (x - (CW - 1) / 2.0f) * s, zi = (y - (CH - 1) / 2.0f) * s;
            int n = 0;
            float m = 0;
            for (; n < MAXI; n++) {
                float r2 = zr * zr, i2 = zi * zi;
                m = r2 + i2;
                if (m > 16.0f) break;
                zi = 2 * zr * zi + ci;
                zr = r2 - i2 + cr;
            }
            rgb_t c;
            if (n == MAXI) c = hsv((uint8_t)(hue0 + 128), 255, 40);  // inside: deep glow
            else {
                float nu = n + 1.0f - log2f(log2f(m) * 0.5f);       // smooth colouring
                float v = sqrtf(MINF(1.0f, nu / 10.0f));
                c = hsv((uint8_t)(hue0 + (int)(nu * 13.0f)), 230, u8c(v));
            }
            canvas_px(x, y, c);
            canvas_px(CW - 1 - x, CH - 1 - y, c);
        }
}

// --------------------------------------------------------------- Metaballs
// Lava-lamp blobs: an energy field from moving balls, solid where the field
// is strong and glowing at the edges; colours mix where blobs merge.
void fx_metaballs(uint32_t st, uint32_t f)
{
    enum { NB = 6 };
    float t = st / 1000.0f;
    float bx[NB], by[NB], br2[NB];
    rgb_t bc[NB];
    for (int i = 0; i < NB; i++) {
        bx[i] = CW / 2.0f + sinf(t * (0.31f + 0.07f * i) + i * 1.7f) * 88.0f;
        by[i] = CH / 2.0f + sinf(t * (0.53f + 0.05f * i) + i * 2.3f) * 9.0f;
        float r = 6.5f + 2.0f * sinf(t * 0.7f + i);
        br2[i] = r * r;
        bc[i] = hsv((uint8_t)(i * 43 + (int)(t * 6)), 255, 255);
    }
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            float F = 0, cr = 0, cg = 0, cb = 0;
            for (int i = 0; i < NB; i++) {
                float dx = x - bx[i], dy = y - by[i];
                float d2 = dx * dx + dy * dy + 1.0f;
                if (d2 > 36.0f * br2[i]) continue;   // contributes < 3 %: skip the divide
                float w = br2[i] / d2;
                F += w; cr += w * bc[i].r; cg += w * bc[i].g; cb += w * bc[i].b;
            }
            if (F < 0.25f) { canvas_px(x, y, RGB(0, 0, 0)); continue; }
            float v = F >= 1.0f ? (F < 1.25f ? 1.0f : 0.8f) : (F - 0.25f) / 0.75f * 0.45f;
            float k = v / F;
            canvas_px(x, y, RGB((uint8_t)MINF(255, cr * k), (uint8_t)MINF(255, cg * k),
                                (uint8_t)MINF(255, cb * k)));
        }
}

// -------------------------------------------------------------- Spirograph
// Four pens drawing hypotrochoid rosettes with fading ink; each rosette's
// pen offset drifts so the pattern keeps changing.
void fx_spirograph(uint32_t st, uint32_t f)
{
    float k = fx_frame_k();
    float keep = powf(0.975f, k);                    // ink fades at the same rate at any fps
    canvas_fade((uint8_t)MINF(255.0f, (1.0f - keep) * 256.0f));
    static const float Rr[4][2] = { {5, 3}, {7, 4}, {8, 3}, {9, 5} };   // R, r
    float t = st / 1000.0f;
    for (int i = 0; i < 4; i++) {
        float R = Rr[i][0], r = Rr[i][1];
        float d = r * (0.6f + 0.5f * sinf(t * 0.11f + i));
        float scale = 13.5f / (R - r + d);
        float cx = (2 * i + 1) * CW / 8.0f, cy = (CH - 1) / 2.0f;
        uint8_t hue = (uint8_t)(i * 64 + (int)(t * 20));
        float step = 0.06f * k;                        // pen speed, frame-rate independent
        for (int s = 0; s < 8; s++) {
            float a0 = A.pen[i], a1 = a0 + step;
            float q = (R - r) / r;
            float x0 = (R - r) * cosf(a0) + d * cosf(q * a0), y0 = (R - r) * sinf(a0) - d * sinf(q * a0);
            float x1 = (R - r) * cosf(a1) + d * cosf(q * a1), y1 = (R - r) * sinf(a1) - d * sinf(q * a1);
            line_aa(cx + x0 * scale, cy + y0 * scale, cx + x1 * scale, cy + y1 * scale,
                    hsv((uint8_t)(hue + (int)(a1 * 3.0f)), 210, 255));
            A.pen[i] = fmodf(a1, TAU_F * r * 10.0f);
        }
    }
}

// --------------------------------------------------------------- Hyperspace
// Stars rushing past in 3D, drawn as streaks; the ship surges in waves and
// gets a kick on every beat.
void fx_hyperspace(uint32_t st, uint32_t f)
{
    enum { N = 90 };
    float k = fx_frame_k();
    if (!A.stars_ok) {
        for (int i = 0; i < N; i++) {
            uint32_t h = hash32(i * 2654435761u + 7);
            A.star[i].x = ((h & 1023) / 512.0f - 1.0f) * 12.0f;
            A.star[i].y = (((h >> 10) & 1023) / 512.0f - 1.0f) * 1.9f;
            A.star[i].z = 0.1f + ((h >> 20) & 1023) / 1024.0f;
        }
        A.stars_ok = true;
    }
    float surge = 0.5f + 0.5f * sinf(st / 2200.0f);
    float v = (0.006f + 0.02f * surge * surge + 0.008f * beat_env(4.0f)) * k;
    float tail = 0.03f + 0.22f * surge * surge;      // streak length, in depth
    canvas_clear();
    for (int i = 0; i < N; i++) {
        float x = A.star[i].x, y = A.star[i].y, z = A.star[i].z - v;
        float zt = z + tail;
        float x0 = CW / 2.0f + x / zt * 8.0f, y0 = CH / 2.0f + y / zt * 8.0f;
        float x1 = CW / 2.0f + x / z * 8.0f, y1 = CH / 2.0f + y / z * 8.0f;
        if (z < 0.03f || x1 < -20 || x1 > CW + 20 || y1 < -10 || y1 > CH + 10) {
            uint32_t h = hash32((uint32_t)(i * 977 + st));
            A.star[i].x = ((h & 1023) / 512.0f - 1.0f) * 12.0f;
            A.star[i].y = (((h >> 10) & 1023) / 512.0f - 1.0f) * 1.9f;
            A.star[i].z = 1.0f;
            continue;
        }
        A.star[i].z = z;
        float b = MINF(1.0f, 0.15f / z);
        uint32_t h = hash32(i);
        rgb_t c = hsv((uint8_t)(140 + (h & 31)), (uint8_t)(h >> 8 & 63), u8c(b));
        line_aa(x0, y0, x1, y1, c);
    }
}
