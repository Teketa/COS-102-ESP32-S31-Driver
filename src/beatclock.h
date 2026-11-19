// Beat clock: a local model of the DJ software's beat, locked to tempo/phase
// reports that arrive over WiFi.
//
// Packets say "deck N is at phase p (0..1 within the current beat) at BPM b".
// They arrive late by a variable amount (WiFi, the bridge, Mixxx's script
// timer), so flashing on packet arrival would look sloppy. Instead the sign
// keeps its own beat counter running at the reported BPM and nudges its phase
// toward the reports, trusting the least-delayed ones most (a late packet
// always reports an *earlier* phase, never a later one).
//
// Pure C, no ESP-IDF dependencies, so it can be unit-tested on a PC.
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     locked;        // have a phase reference
    bool     playing;       // last report said a deck is on air
    double   period_us;     // one beat, microseconds
    double   beats0;        // beat position at t0
    int64_t  t0;            // reference time (us)
    float    bpm;
    float    vu;            // main output level 0..1
    uint8_t  deck;          // on-air deck (1..4), 0 = none
    uint32_t deck_changes;  // increments when the on-air deck changes
    int64_t  last_rx;       // arrival time of the last report (0 = never)
    uint32_t packets;       // reports accepted
    float    err_ms;        // last phase error (diagnostic, + = model behind)
    double   last_beats;    // last position handed out (keeps count monotonic)
    uint8_t  bad;           // consecutive reports far off the model
} beatclock_t;

void bc_init(beatclock_t *c);

// Feed one report received at local time t_rx (us).
void bc_update(beatclock_t *c, int64_t t_rx, float bpm, float phase,
               bool playing, uint8_t deck, float vu);

// Beat position at local time t (us). Returns false when there is no live
// beat (nothing playing, no reports for timeout_us, or never locked).
bool bc_beats_at(beatclock_t *c, int64_t t, int64_t timeout_us, double *beats);
