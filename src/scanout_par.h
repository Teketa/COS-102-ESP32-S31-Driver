// Parallel IO (PARLIO) DMA scan backend -- the ESP32-S31 replacement for the
// S3's LCD_CAM scan engine. Same model and API shape; see scanout_par.c.
#pragma once
#include <stdbool.h>
#include <stdint.h>

// Runtime scan timing. GCLK = pclk_hz / 2. Driver datasheet max is 30 MHz
// GCLK (pclk 60 MHz); the OEM ran 12.5 MHz.
//
// The S31's PARLIO clock comes from a 160 MHz PLL through an integer divider,
// so pclk is snapped to 160/n MHz (80, 53.3, 40, 32, 26.7, 22.9, 20, ...),
// i.e. GCLK 40, 26.7, 20, 16, 13.3, 11.4, 10 MHz... A fractional divider
// would hit any rate, but makes uneven clock periods; GCLK edges stay exact.
typedef struct {
    uint32_t pclk_hz;   // PARLIO sample clock; GCLK is half this
    int burst;          // GCLK pulses per scan phase (OEM 1041)
    int gap1;           // samples of GCLK-low before each burst
    int gap2;           // samples of GCLK-low after each burst
    int pad;            // extra GCLK pulses in the first cycle after vsync
} scan_tuning_t;

extern scan_tuning_t g_tune;
bool  scanout_par_set_tuning(const scan_tuning_t *t);
uint32_t scanout_par_quantize_pclk(uint32_t hz);  // nearest exact sample clock
float scanout_par_refresh_hz(void);       // measured, for display

bool scanout_par_init(void);        // lazy init; true on success
void scanout_par_start(void);
void scanout_par_stop(void);
void scanout_par_pause_wait(void);  // blocks until GCLK idle at cycle boundary
void scanout_par_resume(void);
bool scanout_par_running(void);
uint32_t scanout_par_cycle_count(void);  // completed scan cycles since boot
uint32_t scanout_par_cycle_us(void);     // nominal scan cycle, us (pacing; constant)
void scanout_par_pause_at(uint32_t target_cycle);  // exact-count pause, blocks
