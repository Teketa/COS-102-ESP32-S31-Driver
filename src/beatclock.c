#include "beatclock.h"
#include <math.h>
#include <string.h>

// Phase correction gains. A report that is *ahead* of the model can only mean
// the model is behind (packets are never early), so it is followed quickly;
// a report that is behind is usually just a late packet, so it moves the
// model only a little. Net effect: the model tracks the earliest-arriving
// reports, which are the most accurate.
// Tuned in a simulation of 50 Hz reports over WiFi (3 ms + exponential delay
// with mean 8 ms, 3 % spikes of 40-100 ms): ~0.7 ms rms jitter, 6 ms worst
// case, and a cue jump re-locks within ~200 ms.
#define GAIN_AHEAD   0.10
#define GAIN_BEHIND  0.01
#define SNAP_BEATS   0.20   // errors this big are either one late packet...
#define SNAP_COUNT   4      // ...or, if this many in a row, a real jump (cue)

void bc_init(beatclock_t *c)
{
    memset(c, 0, sizeof(*c));
    c->period_us = 500000.0;   // 120 BPM until told otherwise
    c->bpm = 120.0f;
}

static double model_at(const beatclock_t *c, int64_t t)
{
    return c->beats0 + (double)(t - c->t0) / c->period_us;
}

void bc_update(beatclock_t *c, int64_t t_rx, float bpm, float phase,
               bool playing, uint8_t deck, float vu)
{
    c->last_rx = t_rx;
    c->vu = vu < 0 ? 0 : (vu > 1 ? 1 : vu);
    if (deck && deck != c->deck) {
        if (c->deck) c->deck_changes++;
        c->deck = deck;
        c->locked = false;        // different track: take its phase directly
    }
    c->playing = playing;
    if (!playing || !(bpm >= 30.0f && bpm <= 400.0f) || !(phase >= 0.0f && phase <= 1.0f)) {
        c->locked = false;
        return;
    }
    c->packets++;
    double period = 60e6 / (double)bpm;

    if (!c->locked) {
        // Fresh lock: keep the beat count monotonic so bar-based logic on the
        // sign doesn't see time run backwards.
        double base = floor(c->last_beats);
        double b = base + phase;
        if (b < c->last_beats) b += 1.0;
        c->beats0 = b;
        c->t0 = t_rx;
        c->period_us = period;
        c->bpm = bpm;
        c->locked = true;
        c->bad = 0;
        c->err_ms = 0;
        return;
    }

    double pred = model_at(c, t_rx);
    double frac = pred - floor(pred);
    double err = phase - frac;                    // + = report ahead of model
    if (err > 0.5) err -= 1.0;
    else if (err < -0.5) err += 1.0;
    c->err_ms = (float)(err * period / 1000.0);

    double gain;
    if (fabs(err) > SNAP_BEATS) {
        if (++c->bad < SNAP_COUNT) gain = 0.0;    // one late packet: ignore it
        else { gain = 1.0; c->bad = 0; }          // persistent: re-lock now
    } else {
        c->bad = 0;
        gain = err > 0 ? GAIN_AHEAD : GAIN_BEHIND;
    }
    c->beats0 = pred + err * gain;
    c->t0 = t_rx;
    c->period_us = period;                        // Mixxx's BPM is exact
    c->bpm = bpm;
}

bool bc_beats_at(beatclock_t *c, int64_t t, int64_t timeout_us, double *beats)
{
    if (!c->locked || !c->playing || !c->last_rx || t - c->last_rx > timeout_us)
        return false;
    double b = model_at(c, t);
    if (b > c->last_beats) c->last_beats = b;
    *beats = b;
    return true;
}
