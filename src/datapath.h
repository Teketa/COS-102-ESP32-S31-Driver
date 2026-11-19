// Data-side bus engine: CLK + R/G/B serial data + LAT.
// Bit-banged with direct GPIO register writes. Speed set by dp_set_halfperiod().
// The drivers are SRAM-based: data writes may run at any speed while the
// display side (scanout) keeps the image refreshed.
#pragma once
#include <stdint.h>
#include <stdbool.h>

void dp_init(void);

// Half-period tuning knob: number of busy-loop NOP iterations per CLK half.
// 0 = flat out (~5-8 MHz). Larger = slower. Default 20 (~1.5-2 MHz).
void dp_set_halfperiod(uint32_t nops);
uint32_t dp_get_halfperiod(void);

// Shift n bits from the three word arrays (16-bit words, MSB first, word [0]
// shifted first = ends up at the far end of the chain). No latch.
void dp_shift_words(const uint16_t *r, const uint16_t *g, const uint16_t *b,
                    int nwords);

// Shift the 800-bit chain image and latch it with the given LE code:
//   code 0:  shift all bits, then pulse LE alone in CLK-idle (boot-capture style)
//   code>=1: shift so the last `code` bits are clocked while LE is high and the
//            final payload bit lands on the last rising edge before LE falls
//            (exact OEM alignment; keeps register/data word-aligned at LE fall)
void dp_shift_latch(const uint16_t *r, const uint16_t *g, const uint16_t *b,
                    int nwords, int code);

// Bare LE command with no payload semantics: LE high across `code` CLK edges
// (used for vsync=3 and pre-active=14; shifts `code` junk bits like the OEM).
void dp_latch_only(int code);

// Config write: broadcast one 16-bit value per color line to all 50 drivers,
// latched with `code`.
void dp_config_write(uint16_t r_val, uint16_t g_val, uint16_t b_val, int code);

// Slow diagnostic: toggle each output pin at ~2 Hz for probe identification.
void dp_pin_test(int seconds);
