// Display-side engine: continuous scan (address lines) + GCLK bursts.
// The drivers refresh the panel from internal SRAM using GCLK; this must run
// continuously for anything to light up. Runs as a dedicated task on core 1.
//
// Backend "timer"  = bit-banged GCLK (diagnostics; a few MHz, busy-loops core 1).
// Backend "parlio" = Parallel IO TX DMA replay at OEM-grade rates
//                    (scanout_par.c) -- the normal backend on the S31.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SCAN_BACKEND_TIMER  0   // bit-banged (bring-up; a few MHz GCLK)
#define SCAN_BACKEND_PARLIO 1   // PARLIO TX DMA replay (20 MHz GCLK, zero CPU)

void scanout_init(void);
void scanout_start(void);
void scanout_stop(void);
bool scanout_running(void);
bool scanout_set_backend(int backend);   // switches while stopped
int  scanout_get_backend(void);

// Pause/resume around vsync + config writes (the OEM stops GCLK for ~174 us
// there). Pause blocks until the scan task acknowledges (GCLK idle low).
void scanout_pause(void);
void scanout_resume(void);

// Configuration (take effect next burst)
void scanout_set_burst(uint32_t pulses);      // default 1041 (measured)
uint32_t scanout_get_burst(void);
void scanout_set_halfperiod(uint32_t nops);   // GCLK half-period busy-loop len
void scanout_set_hold(int phase);             // -1 = scan 0..7; 0..7 = freeze on
int  scanout_get_hold(void);                  //   one phase (row-map discovery)

// Manual address control while stopped (discovery)
void scanout_set_addr(int phase);
