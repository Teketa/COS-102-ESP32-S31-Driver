// Framebuffer (200x30, 16-bit grayscale per color) and the SOLVED pixel
// mapping (fitted offline from captures of known display content — see
// docs/PROTOCOL.md §6a). Residual unknowns are runtime knobs:
//   - rf[]: per-channel row-in-half lookup (partially confirmed, rest = best
//     guess following the observed interleave pattern)
//   - mirror_x / flip_y: global orientation (border is symmetric; digits will
//     tell on the real panel)
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define PANEL_W 200
#define PANEL_H 30   // 40x30 populated per board (de-populated 40x32 design)

typedef struct {
    int8_t rf[16];       // channel -> row-in-half (0..15); halves 16 rows apart
    bool mirror_x;
    bool flip_y;
    bool word_lsb_first; // grayscale bit order within the 16-bit word
} map_cfg_t;

extern map_cfg_t g_map;
extern uint16_t g_fb[3][PANEL_H][PANEL_W];  // [color][y][x], 0=R 1=G 2=B

void fb_clear(void);
void fb_set(int x, int y, uint16_t r, uint16_t g, uint16_t b);
// (slot, word) -> pixel under the current map; false if unused/out of range.
bool map_locate(int slot, int word, int *px, int *py);
// Precomputed (slot, word) -> canvas index (y*PANEL_W+x), 0xFFFF = unused.
const uint16_t *map_lut(void);
void map_build_lut(void);
void fb_pack(void);
void map_default(void);
void map_print(void);
bool map_set(const char *key, int val);
bool map_set_rf(int channel, int row);
