// DJ beat sync: UDP beat reports from the Mixxx bridge (mixxx/ in the repo)
// feed a local beat clock (beatclock.c). The render task asks for the beat
// position once per frame and hands it to the effects in g_bf.
//
// Packet (UDP port BEAT_UDP_PORT, little-endian, 28 bytes):
//   0  'C' '1' '0' '2'   magic
//   4  u8  version (1)
//   5  u8  flags         bit0 = a deck is playing on air
//   6  u8  deck          on-air deck 1..4 (0 = none)
//   7  u8  seq           increments per packet
//   8  f32 bpm           rate-adjusted BPM of the on-air deck
//   12 f32 phase         0..1 position within the current beat (beat_distance)
//   16 f32 vu            main output level 0..1
//   20 u64 samplers      bit n-1 set while Mixxx [SamplerN] is playing
#pragma once
#include <stdbool.h>
#include <stdint.h>

#define BEAT_UDP_PORT 4210

typedef struct {
    bool   synced;   // live beat from the DJ software in this frame
    double beats;    // beat position at the moment this frame becomes visible
    float  bpm;      // tempo (120 when free-running)
    float  vu;       // main output level 0..1 (0 when not synced)
} beat_frame_t;

// Written by the render task once per frame, before the effect runs.
extern beat_frame_t g_bf;

typedef struct {
    bool     synced;        // reports arriving and a deck is playing
    float    bpm;
    uint8_t  deck;
    float    vu;
    int32_t  age_ms;        // since the last report, -1 = never
    float    err_ms;        // last phase correction (diagnostic)
    uint32_t packets;       // reports received
    uint32_t deck_changes;
} beat_status_t;

void beatsync_start(void);

// Beat position for a frame that becomes visible at local time t_vis (us).
// Without live reports it free-runs at 120 BPM on the animation clock
// (anim_us, which already includes the speed slider).
void beatsync_frame(int64_t t_vis, uint64_t anim_us, beat_frame_t *out);

void beatsync_status(beat_status_t *st);

// Samplers playing now (bit n-1 = [SamplerN]); 0 when reports have stopped.
// order[] (64 entries, may be NULL) gets a start counter per sampler: the
// highest playing value is the most recently started one. start[] (64, may be
// NULL) gets each sampler's start time (esp_timer us).
uint64_t beatsync_samplers(uint32_t *order, int64_t *start);
int beatsync_sampler_last(void);   // most recently started sampler, 0 = none yet
