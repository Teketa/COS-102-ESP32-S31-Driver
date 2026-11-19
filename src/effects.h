// Animation library: full-panel effects, text animations, and clock mode.
// `st` is a speed-scaled millisecond timebase so animation rate is
// independent of the frame rate.
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef void (*fx_fn)(uint32_t st, uint32_t frame);
typedef void (*textfx_fn)(const char *s, uint32_t st, uint32_t frame);

// beat = 1: a beat-synced animation (follows the Mixxx beat; free-runs at
// 120 BPM otherwise). Used for grouping in the UI and the DJ auto-change pool.
// art = 1: a 3D / shapes effect, 2: horses & party theme (UI groups).
typedef struct { const char *name; fx_fn fn; uint8_t beat; uint8_t art; } effect_t;
typedef struct { const char *name; textfx_fn fn; } textanim_t;

extern const effect_t g_effects[];
extern const int g_effect_count;
extern const textanim_t g_textanims[];
extern const int g_textanim_count;

void fx_reset(void);                 // clear per-effect state (on switch)
// Next effect index from `cur` in direction dir (+1/-1), optionally only
// beat-synced ones. Wraps.
int fx_next(int cur, int dir, bool beat_only);
// 30-fps reference frames of animation in the current frame (speed slider,
// tempo lock and pause included) -- for effects that step a simulation.
float fx_frame_k(void);
// Whole pixels travelled at an even frame cadence (see motion_advance in
// effects.c). Call once per frame; resets when the effect changes.
int32_t fx_motion(float px_per_s);
// Shared radial tables (row-major, PANEL_W per row) around the panel centre,
// vertical distances x3: distance 0..~110 and angle 0..255.
const uint8_t *fx_radial_rad(void);
const uint8_t *fx_radial_ang(void);
void fx_render(int idx, uint32_t st, uint32_t frame);
void textfx_render(int idx, const char *s, uint32_t st, uint32_t frame);
void clock_render(uint32_t st, uint32_t frame);
