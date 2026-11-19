// Persisted configuration (NVS). One flat struct, versioned.
#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SETTINGS_VERSION 2
#define TEXT_MAX 192

enum { MODE_EFFECT = 0, MODE_TEXT, MODE_CLOCK, MODE_OFF };

typedef struct {
    uint16_t version;
    char ssid[33];
    char pass[65];
    uint8_t mode;
    uint8_t effect;         // effect index (MODE_EFFECT)
    uint8_t text_anim;      // text animation index (MODE_TEXT)
    uint8_t brightness;     // 0..255
    uint8_t speed;          // 1..100 (animation rate)
    uint8_t fps;            // 5..60 frame rate
    uint8_t fg[3];          // foreground / primary color
    uint8_t bg[3];          // background color
    uint8_t font_scale;     // 1..4
    bool    paused;
    bool    playlist;       // auto-advance effects
    uint16_t playlist_secs;
    bool    clock24;        // 24h vs 12h
    bool    clock_seconds;
    char    tz[40];         // POSIX TZ string
    char    text[TEXT_MAX];
    // Bus tuning (overclocking), persisted so it survives a reboot.
    uint32_t tune_pclk_hz;  // i80 pixel clock; GCLK = half
    uint32_t tune_data_hz;  // quad-SPI data clock
    uint16_t tune_burst;    // GCLK pulses per scan phase
    uint8_t  tune_pad;      // extra pulses in the first cycle after vsync
    uint8_t  wifi_txpower;  // esp_wifi units (0.25 dBm); 0 = default 52 (13 dBm)
} settings_t;

extern settings_t g_set;

// DJ beat sync (Mixxx -> bridge -> UDP). Stored as its own NVS record so it
// can grow without invalidating the main settings (and your WiFi details).
#define BEAT_CFG_VERSION 1
typedef struct {
    uint16_t version;
    bool    enabled;        // follow beat reports from the Mixxx bridge
    int16_t offset_ms;      // + = show beats later (audio output latency)
    uint8_t pulse;          // 0..100 % brightness pulse on every beat, any animation
    uint8_t auto_bars;      // switch animation every N bars (0 = off)
    uint8_t auto_pool;      // 0 = beat animations only, 1 = all animations
    bool    on_deck;        // switch animation when the on-air deck changes
    bool    tempo_lock;     // animation speed follows the track's BPM
} beat_cfg_t;

extern beat_cfg_t g_beat;
void beat_cfg_defaults(void);

void settings_init(void);     // load from NVS or defaults
void settings_save(void);     // persist (debounced by caller)
void settings_defaults(void);
