// Frame-level protocol: boot init, per-frame preamble, data streaming.
// Mirrors the decoded OEM behavior (docs/PROTOCOL.md) with every quirk
// adjustable so each assumption can be tested on the real panel.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "replay_data.h"

typedef struct {
    int  n_data_latches;       // per frame; OEM sends 127 (128 SRAM slots)
    int  data_latch_code;      // 1 = OEM steady style, 0 = idle-pulse style
    bool preamble_each_frame;  // OEM: true (vsync + both config writes)
    bool stop_gclk_in_preamble;// OEM: true (GCLK quiet ~174us around preamble)
    uint16_t cfg_a;            // code-10 payload (OEM 0x03EA)
    uint16_t cfg_b_r, cfg_b_g, cfg_b_b; // code-8 payloads (OEM D893/3493/F393)
} proto_cfg_t;

extern proto_cfg_t g_proto;

#define DATA_BACKEND_BITBANG 0
#define DATA_BACKEND_QSPI    1
extern int g_data_backend;
extern uint32_t g_proto_send_us;   // last data-send duration, microseconds
extern uint32_t g_proto_late_frames;  // vsyncs that missed the nominal grid
extern uint32_t g_proto_frames;       // vsyncs emitted

// Slot buffer: the exact 800-bit chain image for each of the 128 SRAM write
// positions, per color. slot[s].r[w] is the w-th 16-bit word shifted (w=0
// first-shifted = far end of chain = board 5 side).
typedef struct {
    uint16_t r[WORDS_PER_SHIFT];
    uint16_t g[WORDS_PER_SHIFT];
    uint16_t b[WORDS_PER_SHIFT];
} slot_t;

extern slot_t g_slots[SRAM_SLOTS];

void proto_init(void);

// Panel bus lock — held by anything driving CLK/LAT/data or reconfiguring a
// backend (render task, console commands, web API). Recursive.
void proto_lock(void);
void proto_unlock(void);

// One-time boot sequence (capture5): config write code 4, display stopped.
void proto_boot(void);

// vsync + config writes, honoring stop_gclk_in_preamble.
void proto_preamble(void);
void proto_vsync_only(void);

// Send data latches from the slot buffer: slots [first, first+count).
void proto_send_slots(int first, int count);

// Full frame: preamble (if enabled) + n_data_latches slots from slot 0.
void proto_send_frame(void);

// Base priority of whichever task calls proto_send_frame_paced() (the render
// task). It is briefly boosted around the vsync and restored to this.
#define PROTO_SENDER_PRIO 6

// Send one frame paced to fps; *next_cycle is caller-owned state (init 0).
void proto_send_frame_paced(uint32_t *next_cycle, int fps);

// Continuous streaming task control.
void proto_auto_start(int fps);
void proto_auto_stop(void);
bool proto_auto_running(void);
