// Sample texts: while a Mixxx sampler plays (e.g. a sample pad on the DJ
// controller), show a text set for that sampler -- "SHOTS!" while Lil Jon
// shouts it -- then go straight back to whatever was showing.
//
// Sampler play states arrive with the beat reports from the Mixxx bridge
// (beatsync.c). Texts are configured per Mixxx sampler number in the web UI
// (Samples tab), which also shows the number of the last pad you pressed.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SAMPLE_SLOTS     32     // Mixxx [Sampler1] .. [Sampler32]
#define SAMPLE_TEXT_MAX  24     // incl. terminator

// Stored by number in NVS: only ever append.
enum { SMP_SOLID, SMP_BEAT, SMP_RAINBOW, SMP_STROBE,
       SMP_MARQUEE, SMP_REVEAL, SMP_DROP, SMP_PUNCH, SMP_WAVE, SMP_GLITCH,
       SMP_STYLE_COUNT };
extern const char *const g_sample_style_names[SMP_STYLE_COUNT];

typedef struct {
    char    text[SAMPLE_TEXT_MAX];   // empty = nothing shown for this sampler
    uint8_t rgb[3];
    uint8_t style;                   // SMP_*
} sample_slot_t;

#define SAMPLES_CFG_VERSION 1
typedef struct {
    uint16_t      version;
    bool          enabled;
    sample_slot_t slot[SAMPLE_SLOTS];
} samples_cfg_t;

extern samples_cfg_t g_samples;
void samples_defaults(void);

// The text to show this frame: the most recently started sampler that is
// still playing and has a text. NULL = none. *sampler gets its number and
// *ms how long ago it started (either may be NULL).
const sample_slot_t *samples_active(int *sampler, uint32_t *ms);

// Show sampler n's text for a moment as if its pad was hit (web UI preview).
void samples_preview(int n);

// Draw a sample text full-panel (sharp, largest size that fits). `st` is the
// animation clock, `ms` real time since the pad was hit (intros start at 0).
void samples_render(const sample_slot_t *s, uint32_t st, uint32_t ms);
