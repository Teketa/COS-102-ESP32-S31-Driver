// Test patterns. Two families:
//  - raw_*: write g_slots[] directly (no mapping involved) — for protocol
//    discovery: light a specific (slot, word, bit) and observe where it lands.
//  - fb_*: draw into the framebuffer, then fb_pack() — for validating a
//    mapping hypothesis end to end.
#pragma once
#include <stdint.h>

void raw_clear(void);
// Fill every word of every slot with one value per color.
void raw_fill(uint16_t r, uint16_t g, uint16_t b);
// Fill a single slot (all 50 words), other slots untouched.
void raw_fill_slot(int slot, uint16_t r, uint16_t g, uint16_t b);
// Set one word of one slot.
void raw_set_word(int slot, int word, uint16_t r, uint16_t g, uint16_t b);

void fbpat_solid(uint16_t r, uint16_t g, uint16_t b);
void fbpat_rows(void);      // each row a distinct brightness ramp step
void fbpat_cols(void);
void fbpat_grad_x(void);
void fbpat_checker(int cell);
void fbpat_border(void);
void fbpat_pixel(int x, int y, uint16_t r, uint16_t g, uint16_t b);
