#include "framebuf.h"
#include "protocol.h"
#include <string.h>
#include <stdio.h>

map_cfg_t g_map;
uint16_t g_fb[3][PANEL_H][PANEL_W];

// (slot, word) -> canvas index lookup, rebuilt whenever the mapping changes.
static uint16_t s_lut[SRAM_SLOTS][WORDS_PER_SHIFT];
static bool s_lut_ok;

// ---------------------------------------------------------------------------
// SOLVED mapping (fitted offline from captures 20-22, known clock content;
// see analysis/fit_mapping*.py and docs/PROTOCOL.md §6a):
//
//   slot s (0..127)  ->  channel c = s/8,  phase p = s%8
//   word w (0..49)   ->  chain board b = w/10 (b=4 ... board 1, leftmost),
//                        local l = w%10, chip-row q = l/2, side t = l%2
//   PCB col of chip  ->  from traced zigzag (q,t): [(0,3),(3,0),(0,3),(3,0),(0,3)]
//
//   x = (4-b)*40 + (4-q)*8 + xf      xf = p on even channels, 7-p on odd
//                                    (serpentine), optionally mirrored
//   y = half*15 + rf[c]              half: PCB-col-3 chip = top (rows 0-14),
//                                    PCB-col-0 = bottom (15-29); optional flip
//   rf[c]: per-channel row-in-half, 15 of 16 channels used.
//
// Verified from captures: c1,c3,c5 -> rows 0..3 (top border), c4 -> 11..14
// (bottom border), c0,c2,c15 -> strip rows ~6..8. Exact permutation of the
// remaining channels is set here as the best guess consistent with all
// captures — confirm/adjust with `map rf <c> <row>` on the real panel.
// ---------------------------------------------------------------------------

static const int8_t PCB_COL[5][2] = {{0,3},{3,0},{0,3},{3,0},{0,3}};

// Channel -> row-in-half. MEASURED on hardware via full ws walk (2026-07-26):
// even channels -> row 8 + c/2, odd channels -> row (c-1)/2; halves 16 rows
// apart (32 logical rows, physical 30; rows 30/31 unpopulated -> channels 12
// and 14 have no bottom partner, culled by the bounds check).
static const int8_t RF_DEFAULT[16] =
    { 8, 0, 9, 1, 10, 2, 11, 3, 12, 4, 13, 5, 14, 6, 15, 7 };

void map_default(void)
{
    g_map = (map_cfg_t){ .mirror_x = false, .flip_y = false,
                         .word_lsb_first = false };
    memcpy(g_map.rf, RF_DEFAULT, sizeof(g_map.rf));
    s_lut_ok = false;
}

void fb_clear(void) { memset(g_fb, 0, sizeof(g_fb)); }

void fb_set(int x, int y, uint16_t r, uint16_t g, uint16_t b)
{
    if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) return;
    g_fb[0][y][x] = r; g_fb[1][y][x] = g; g_fb[2][y][x] = b;
}

// Built once per mapping change: recomputing the mapping every frame (6400
// calls with integer divides) was a big slice of the frame budget, and an
// overrun rebases the vsync pacing — visible as stutter and brightness pulsing.
void map_build_lut(void)
{
    for (int s = 0; s < SRAM_SLOTS; s++)
        for (int w = 0; w < WORDS_PER_SHIFT; w++) {
            int x, y;
            s_lut[s][w] = map_locate(s, w, &x, &y) ? (uint16_t)(y * PANEL_W + x)
                                                   : 0xFFFF;
        }
    s_lut_ok = true;
}

const uint16_t *map_lut(void)
{
    if (!s_lut_ok) map_build_lut();
    return &s_lut[0][0];
}

bool map_locate(int slot, int word, int *px, int *py)
{
    int c = slot / 8, p = slot % 8;
    int b = word / 10, l = word % 10, q = l / 2, t = l % 2;
    if (g_map.rf[c] < 0) return false;
    int xf = (c & 1) ? (7 - p) : p;
    int x = (4 - b) * 40 + (4 - q) * 8 + xf;
    int half_top = (PCB_COL[q][t] == 3);   // col-3 chip = top half
    int y = (half_top ? 0 : 16) + g_map.rf[c];   // halves 16 apart (rows 30/31 absent)
    if (g_map.mirror_x) x = PANEL_W - 1 - x;
    if (g_map.flip_y) y = PANEL_H - 1 - y;
    if (x < 0 || x >= PANEL_W || y < 0 || y >= PANEL_H) return false;
    *px = x; *py = y;
    return true;
}

static uint16_t maybe_rev(uint16_t v)
{
    if (!g_map.word_lsb_first) return v;
    uint16_t o = 0;
    for (int i = 0; i < 16; i++) if (v & (1 << i)) o |= 0x8000 >> i;
    return o;
}

void fb_pack(void)
{
    for (int s = 0; s < SRAM_SLOTS; s++) {
        for (int w = 0; w < WORDS_PER_SHIFT; w++) {
            int x, y;
            if (!map_locate(s, w, &x, &y)) {
                // unused channel or out of range: fully off. (The OEM's 0x0002
                // black level is visible as periodic blips at slow GCLK.)
                g_slots[s].r[w] = g_slots[s].g[w] = g_slots[s].b[w] = 0x0000;
                continue;
            }
            g_slots[s].r[w] = maybe_rev(g_fb[0][y][x]);
            g_slots[s].g[w] = maybe_rev(g_fb[1][y][x]);
            g_slots[s].b[w] = maybe_rev(g_fb[2][y][x]);
        }
    }
}

void map_print(void)
{
    printf("map: mirror_x=%d flip_y=%d lsbfirst=%d\nrf:", g_map.mirror_x,
           g_map.flip_y, g_map.word_lsb_first);
    for (int c = 0; c < 16; c++) printf(" %d", g_map.rf[c]);
    printf("\n");
}

bool map_set(const char *key, int val)
{
    if (!strcmp(key, "mirrorx"))  { g_map.mirror_x = val; s_lut_ok = false; return true; }
    if (!strcmp(key, "flipy"))    { g_map.flip_y = val;   s_lut_ok = false; return true; }
    if (!strcmp(key, "lsbfirst")) { g_map.word_lsb_first = val; return true; }
    return false;
}

bool map_set_rf(int channel, int row)
{
    if (channel < 0 || channel > 15 || row < -1 || row > 15) return false;
    g_map.rf[channel] = (int8_t)row;
    s_lut_ok = false;
    return true;
}
