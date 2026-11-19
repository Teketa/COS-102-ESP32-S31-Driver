// Measured protocol constants, decoded from the original controller's traffic.
// Sources: Captures 2,3,4,5,6,7,8,11,12,13 (see analysis/ and docs/PROTOCOL.md).
#pragma once
#include <stdint.h>

// --- chain geometry (traced + measured) ---
#define CHAIN_DRIVERS        50    // SUM2030 per color line (10/board x 5 boards)
#define BITS_PER_SHIFT       800   // 50 x 16-bit words per color per latch
#define WORDS_PER_SHIFT      50
#define SCAN_PHASES          8     // address 0..7
// SOLVED on hardware (2026-07-26): the SRAM write pointer is NOT reset by
// vsync; it free-runs mod 128, and exactly one preamble command advances it
// too. Hence the OEM's 127 data latches: 1 (preamble) + 127 = 128 = stable.
// Sending 128 makes the image walk by one channel per frame (observed).
#define DATA_LATCHES_MEAS    127
#define SRAM_SLOTS           128   // 16 channels x 8 scan rows

// --- latch codes (LE width in CLK rising edges) ---
#define LCODE_DATA           1     // data latch (0-edge "idle latch" also works, seen in boot)
#define LCODE_VSYNC          3     // frame buffer swap
#define LCODE_CFG_BOOT       4     // boot-only config write (payload 0x03EA)
#define LCODE_CFG_B          8     // per-frame config write B (per-color payload)
#define LCODE_CFG_A          10    // per-frame config write A (payload 0x03EA)
#define LCODE_PREACTIVE      14    // precedes each config write

// --- measured config payloads (broadcast x50 to the whole chain) ---
#define CFG_A_VALUE          0x03EA  // via code 10 each frame, and code 4 at boot
#define CFG_B_R              0xD893  // via code 8, R line   (likely current gain /
#define CFG_B_G              0x3493  //              G line    white balance)
#define CFG_B_B              0xF393  //              B line

// --- original timing (replay reference; drivers tolerate deviations) ---
#define OEM_CLK_HZ           12500000
#define OEM_GCLK_HZ          12500000
#define OEM_GCLK_BURST       1041    // GCLK pulses per scan row
#define OEM_SCAN_ROW_US      86.24f  // burst + ~3.0us quiet (addr changes in the gap)
#define OEM_LATCH_SLOT_US    126.19f
#define OEM_FRAME_US         16729.8f // 59.77 Hz
#define OEM_TAIL_SLOTS       3.5f     // CLK quiet before vsync
#define OEM_PREAMBLE_GCLK_QUIET_US 174.0f // GCLK stopped around vsync+config
