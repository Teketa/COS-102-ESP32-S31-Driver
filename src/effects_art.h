// Artistic effects: 3D wireframes and shaded objects, a 4D tesseract,
// fractals, blobs and drawn curves. Same signature as every other effect:
// `st` is the speed-scaled animation clock in ms.
#pragma once
#include <stdint.h>

void art_reset(void);    // called from fx_reset() on every effect switch

void fx_retro_sun(uint32_t st, uint32_t f);
void fx_solids(uint32_t st, uint32_t f);
void fx_tesseract(uint32_t st, uint32_t f);
void fx_donuts(uint32_t st, uint32_t f);
void fx_disco_ball(uint32_t st, uint32_t f);
void fx_demo_tunnel(uint32_t st, uint32_t f);
void fx_wave_mesh(uint32_t st, uint32_t f);
void fx_julia(uint32_t st, uint32_t f);
void fx_metaballs(uint32_t st, uint32_t f);
void fx_spirograph(uint32_t st, uint32_t f);
void fx_hyperspace(uint32_t st, uint32_t f);
