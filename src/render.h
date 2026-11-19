// Render engine: owns the frame loop (effect -> canvas -> gamma/brightness ->
// panel mapping -> paced frame send). Single owner of the panel while running.
#pragma once
#include <stdint.h>
#include <stdbool.h>

void render_init(void);      // starts the render task (idle until started)
void render_start(void);
void render_stop(void);
bool render_active(void);

void render_next_effect(int delta);   // +1 / -1, wraps
float render_speed(void);
void render_snapshot(uint8_t *rgb);   // CW*CH*3 bytes: the frame on the panel             // speed slider x tempo lock (1.0 = normal)
void render_touch(void);              // effect/mode changed: reset fx state
uint32_t render_frame_count(void);
uint32_t render_canvas_hash(void);   // change detection for the live preview
int render_fps_effective(void);      // rate actually being paced (<= setting)
float render_fps_actual(void);
// Per-frame timing breakdown (microseconds), for spotting deadline overruns.
uint32_t render_render_us(void);   // effect/text rendering into the canvas
uint32_t render_pack_us(void);     // canvas -> driver slots
uint32_t render_send_us(void);     // data latches out on the bus
