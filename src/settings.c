#include "settings.h"
#include "samples.h"
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

#define TAG "settings"
#define NVS_NS  "cos102"
#define NVS_KEY "cfg"
#define NVS_KEY_BEAT "beat"
#define NVS_KEY_SAMPLES "samples"

settings_t g_set;
beat_cfg_t g_beat;

void beat_cfg_defaults(void)
{
    memset(&g_beat, 0, sizeof(g_beat));
    g_beat.version = BEAT_CFG_VERSION;
    g_beat.enabled = true;
    // Mixxx's audio reaches the speakers a little after its beat position is
    // reported; start by showing beats 20 ms later and tune by eye/ear.
    g_beat.offset_ms = 20;
    g_beat.pulse = 0;
    g_beat.auto_bars = 0;
    g_beat.auto_pool = 0;
    g_beat.on_deck = false;
    g_beat.tempo_lock = false;
}

void settings_defaults(void)
{
    memset(&g_set, 0, sizeof(g_set));
    g_set.version = SETTINGS_VERSION;
    g_set.mode = MODE_EFFECT;
    g_set.effect = 0;
    g_set.text_anim = 1;          // scroll left
    g_set.brightness = 140;
    g_set.speed = 50;
    g_set.fps = 30;
    g_set.fg[0] = 255; g_set.fg[1] = 255; g_set.fg[2] = 255;
    g_set.bg[0] = 0; g_set.bg[1] = 0; g_set.bg[2] = 0;
    g_set.font_scale = 3;
    g_set.playlist = false;
    g_set.playlist_secs = 20;
    g_set.clock24 = true;
    g_set.clock_seconds = true;
    strcpy(g_set.tz, "CET-1CEST,M3.5.0,M10.5.0/3");   // Europe/Copenhagen
    strcpy(g_set.text, "HELLO");
    // Bus defaults: 20 MHz on both clocks — measured stable on this panel
    // (the OEM ran 12.5 MHz; the ceiling is the panel's HC244 buffers and
    // ribbon, not the drivers' 30 MHz rating). `oc reset` returns to the
    // OEM-matched 12.5 MHz for debugging.
    g_set.tune_pclk_hz = 40000000;     // GCLK 20 MHz
    g_set.tune_data_hz = 20000000;
    g_set.tune_burst = 1041;
    g_set.tune_pad = 3;
    g_set.wifi_txpower = 52;      // 13 dBm
}

void settings_init(void)
{
    settings_defaults();
    beat_cfg_defaults();
    samples_defaults();
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
        return;
    }
    beat_cfg_t btmp;
    size_t blen = sizeof(btmp);
    if (nvs_get_blob(h, NVS_KEY_BEAT, &btmp, &blen) == ESP_OK &&
        blen == sizeof(btmp) && btmp.version == BEAT_CFG_VERSION)
        g_beat = btmp;
    // Sample texts: ~900 bytes, read straight into place if the size matches.
    static samples_cfg_t stmp;
    size_t slen = sizeof(stmp);
    if (nvs_get_blob(h, NVS_KEY_SAMPLES, &stmp, &slen) == ESP_OK &&
        slen == sizeof(stmp) && stmp.version == SAMPLES_CFG_VERSION)
        g_samples = stmp;
    settings_t tmp;
    size_t len = sizeof(tmp);
    esp_err_t err = nvs_get_blob(h, NVS_KEY, &tmp, &len);
    nvs_close(h);
    if (err == ESP_OK && len == sizeof(tmp) && tmp.version == SETTINGS_VERSION) {
        g_set = tmp;
        ESP_LOGI(TAG, "settings loaded (ssid='%s', mode=%d)", g_set.ssid, g_set.mode);
    } else {
        ESP_LOGI(TAG, "stored settings invalid/outdated, using defaults");
    }
}

void settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed, not saving");
        return;
    }
    g_set.version = SETTINGS_VERSION;
    g_beat.version = BEAT_CFG_VERSION;
    bool ok = nvs_set_blob(h, NVS_KEY, &g_set, sizeof(g_set)) == ESP_OK;
    ok = (nvs_set_blob(h, NVS_KEY_BEAT, &g_beat, sizeof(g_beat)) == ESP_OK) && ok;
    g_samples.version = SAMPLES_CFG_VERSION;
    ok = (nvs_set_blob(h, NVS_KEY_SAMPLES, &g_samples, sizeof(g_samples)) == ESP_OK) && ok;
    if (ok) nvs_commit(h);
    nvs_close(h);
}
