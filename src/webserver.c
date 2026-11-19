// HTTP UI + REST API + WebSocket live preview.
#include "webserver.h"
#include "settings.h"
#include "render.h"
#include "effects.h"
#include "canvas.h"
#include "wifi_mgr.h"
#include "framebuf.h"
#include "console.h"
#include "scanout.h"
#include "scanout_par.h"
#include "datapath.h"
#include "datapath_qspi.h"
#include "protocol.h"
#include "beatsync.h"
#include "samples.h"
#include <string.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#define TAG "web"
#define MAX_WS_CLIENTS 3
#define STATE_PUSH_MS 2000

// Generated from src/web/index.html by tools/gen_web.py
extern const char web_index_html[];
extern const size_t web_index_html_len;

static httpd_handle_t s_srv;
static int s_ws_fds[MAX_WS_CLIENTS];

// ------------------------------------------------------------------ helpers
static esp_err_t send_json(httpd_req_t *r, cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    esp_err_t e = httpd_resp_sendstr(r, s ? s : "{}");
    cJSON_free(s);
    cJSON_Delete(root);
    return e;
}

static cJSON *read_json_body(httpd_req_t *r)
{
    int len = r->content_len;
    if (len <= 0 || len > 4096) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(r, buf + got, len - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static void rgb_from_json(cJSON *arr, uint8_t out[3])
{
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 3) return;
    for (int i = 0; i < 3; i++) {
        cJSON *v = cJSON_GetArrayItem(arr, i);
        if (cJSON_IsNumber(v)) out[i] = (uint8_t)v->valueint;
    }
}

static cJSON *rgb_to_json(const uint8_t c[3])
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < 3; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(c[i]));
    return a;
}

static cJSON *build_state(bool with_lists)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "mode", g_set.mode);
    cJSON_AddNumberToObject(j, "effect", g_set.effect);
    cJSON_AddNumberToObject(j, "text_anim", g_set.text_anim);
    cJSON_AddNumberToObject(j, "brightness", g_set.brightness);
    cJSON_AddNumberToObject(j, "speed", g_set.speed);
    cJSON_AddNumberToObject(j, "fps", g_set.fps);
    cJSON_AddNumberToObject(j, "font_scale", g_set.font_scale);
    cJSON_AddBoolToObject(j, "paused", g_set.paused);
    cJSON_AddBoolToObject(j, "playlist", g_set.playlist);
    cJSON_AddNumberToObject(j, "playlist_secs", g_set.playlist_secs);
    cJSON_AddBoolToObject(j, "clock24", g_set.clock24);
    cJSON_AddBoolToObject(j, "clock_seconds", g_set.clock_seconds);
    cJSON_AddStringToObject(j, "tz", g_set.tz);
    cJSON_AddStringToObject(j, "text", g_set.text);
    cJSON_AddItemToObject(j, "fg", rgb_to_json(g_set.fg));
    cJSON_AddItemToObject(j, "bg", rgb_to_json(g_set.bg));
    cJSON_AddNumberToObject(j, "w", PANEL_W);
    cJSON_AddNumberToObject(j, "h", PANEL_H);
    cJSON_AddBoolToObject(j, "ap_mode", wifi_is_ap_mode());
    cJSON_AddStringToObject(j, "ip", wifi_ip_str());
    cJSON_AddStringToObject(j, "ssid", wifi_ssid_str());
    cJSON_AddNumberToObject(j, "rssi", wifi_rssi());
    cJSON_AddBoolToObject(j, "time_ok", time_is_valid());
    cJSON_AddNumberToObject(j, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(j, "heap", (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(j, "fps_actual", render_fps_actual());
    cJSON_AddBoolToObject(j, "ui_active", render_active());
    cJSON_AddNumberToObject(j, "gclk_mhz", g_tune.pclk_hz / 2e6);
    cJSON_AddNumberToObject(j, "data_mhz", dp_qspi_get_clock() / 1e6);
    cJSON_AddNumberToObject(j, "burst", g_tune.burst);
    cJSON_AddNumberToObject(j, "gap1", g_tune.gap1);
    cJSON_AddNumberToObject(j, "gap2", g_tune.gap2);
    cJSON_AddNumberToObject(j, "pad", g_tune.pad);
    cJSON_AddNumberToObject(j, "refresh_hz", scanout_par_refresh_hz());
    // Advanced-tab live state
    cJSON_AddNumberToObject(j, "scan_backend", scanout_get_backend());
    cJSON_AddBoolToObject(j, "scan_running", scanout_running());
    cJSON_AddNumberToObject(j, "scan_hold", scanout_get_hold());
    cJSON_AddNumberToObject(j, "data_backend", g_data_backend);
    cJSON_AddNumberToObject(j, "niborder", dp_qspi_get_niborder() ? 1 : 0);
    cJSON_AddNumberToObject(j, "nlat", g_proto.n_data_latches);
    cJSON_AddNumberToObject(j, "latchcode", g_proto.data_latch_code);
    cJSON_AddNumberToObject(j, "gclkstop", g_proto.stop_gclk_in_preamble ? 1 : 0);
    cJSON_AddNumberToObject(j, "preamble_each", g_proto.preamble_each_frame ? 1 : 0);
    cJSON_AddNumberToObject(j, "map_mirrorx", g_map.mirror_x ? 1 : 0);
    cJSON_AddNumberToObject(j, "map_flipy", g_map.flip_y ? 1 : 0);
    cJSON_AddNumberToObject(j, "map_lsbfirst", g_map.word_lsb_first ? 1 : 0);
    cJSON_AddNumberToObject(j, "dspeed", (double)dp_get_halfperiod());
    cJSON_AddNumberToObject(j, "t_render", render_render_us() / 1000.0);
    cJSON_AddNumberToObject(j, "t_pack", render_pack_us() / 1000.0);
    cJSON_AddNumberToObject(j, "t_send", render_send_us() / 1000.0);
    cJSON_AddNumberToObject(j, "fps_eff", render_fps_effective());
    cJSON_AddNumberToObject(j, "late", g_proto_late_frames);
    cJSON_AddNumberToObject(j, "frames", g_proto_frames);
    beat_status_t bs;
    beatsync_status(&bs);
    cJSON_AddBoolToObject(j, "beat_enabled", g_beat.enabled);
    cJSON_AddNumberToObject(j, "beat_offset", g_beat.offset_ms);
    cJSON_AddNumberToObject(j, "beat_pulse", g_beat.pulse);
    cJSON_AddNumberToObject(j, "beat_auto_bars", g_beat.auto_bars);
    cJSON_AddNumberToObject(j, "beat_auto_pool", g_beat.auto_pool);
    cJSON_AddBoolToObject(j, "beat_on_deck", g_beat.on_deck);
    cJSON_AddBoolToObject(j, "beat_tempo_lock", g_beat.tempo_lock);
    cJSON_AddBoolToObject(j, "beat_synced", bs.synced);
    cJSON_AddNumberToObject(j, "beat_bpm", bs.bpm);
    cJSON_AddNumberToObject(j, "beat_deck", bs.deck);
    cJSON_AddNumberToObject(j, "beat_age", bs.age_ms);
    cJSON_AddNumberToObject(j, "beat_err", bs.err_ms);
    cJSON_AddNumberToObject(j, "beat_packets", bs.packets);
    if (with_lists) {
        cJSON *bfx = cJSON_CreateArray();
        for (int i = 0; i < g_effect_count; i++)
            if (g_effects[i].beat) cJSON_AddItemToArray(bfx, cJSON_CreateNumber(i));
        cJSON_AddItemToObject(j, "beat_fx", bfx);
        cJSON *afx = cJSON_CreateArray();
        for (int i = 0; i < g_effect_count; i++)
            if (g_effects[i].art == 1) cJSON_AddItemToArray(afx, cJSON_CreateNumber(i));
        cJSON_AddItemToObject(j, "art_fx", afx);
        cJSON *tfx = cJSON_CreateArray();
        for (int i = 0; i < g_effect_count; i++)
            if (g_effects[i].art == 2) cJSON_AddItemToArray(tfx, cJSON_CreateNumber(i));
        cJSON_AddItemToObject(j, "theme_fx", tfx);
        cJSON *fx = cJSON_CreateArray();
        for (int i = 0; i < g_effect_count; i++)
            cJSON_AddItemToArray(fx, cJSON_CreateString(g_effects[i].name));
        cJSON_AddItemToObject(j, "effects", fx);
        cJSON *ta = cJSON_CreateArray();
        for (int i = 0; i < g_textanim_count; i++)
            cJSON_AddItemToArray(ta, cJSON_CreateString(g_textanims[i].name));
        cJSON_AddItemToObject(j, "textanims", ta);
    }
    return j;
}

// Hand-rolled state JSON for the periodic WebSocket push. cJSON does ~80
// malloc/free per build; doing that every few seconds on core 0 was enough to
// disturb the panel on core 1 (shared heap lock + SRAM bandwidth against the
// scan's continuous DMA stream). snprintf into a static buffer costs neither.
static void json_escape(char *dst, size_t n, const char *src)
{
    size_t o = 0;
    for (; *src && o + 2 < n; src++) {
        if (*src == '"' || *src == '\\') { dst[o++] = '\\'; dst[o++] = *src; }
        else if ((unsigned char)*src >= 0x20) dst[o++] = *src;
    }
    dst[o] = 0;
}

static size_t state_json(char *b, size_t n)
{
    char tx[TEXT_MAX * 2], tz[96];
    json_escape(tx, sizeof(tx), g_set.text);
    json_escape(tz, sizeof(tz), g_set.tz);
    const bool par = scanout_get_backend() == SCAN_BACKEND_PARLIO;
    beat_status_t bs;
    beatsync_status(&bs);
    return (size_t)snprintf(b, n,
        "{\"mode\":%d,\"effect\":%d,\"text_anim\":%d,\"brightness\":%d,"
        "\"speed\":%d,\"fps\":%d,\"font_scale\":%d,\"paused\":%s,"
        "\"playlist\":%s,\"playlist_secs\":%d,\"clock24\":%s,"
        "\"clock_seconds\":%s,\"tz\":\"%s\",\"text\":\"%s\","
        "\"fg\":[%d,%d,%d],\"bg\":[%d,%d,%d],\"w\":%d,\"h\":%d,"
        "\"ap_mode\":%s,\"ip\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,"
        "\"time_ok\":%s,\"uptime\":%lld,\"heap\":%u,\"fps_actual\":%.1f,"
        "\"ui_active\":%s,\"gclk_mhz\":%.2f,\"data_mhz\":%.2f,\"burst\":%d,"
        "\"gap1\":%d,\"gap2\":%d,\"pad\":%d,\"refresh_hz\":%.0f,"
        "\"scan_backend\":%d,\"scan_running\":%s,\"scan_hold\":%d,"
        "\"data_backend\":%d,\"niborder\":%d,\"nlat\":%d,\"latchcode\":%d,"
        "\"gclkstop\":%d,\"preamble_each\":%d,\"map_mirrorx\":%d,"
        "\"map_flipy\":%d,\"map_lsbfirst\":%d,\"dspeed\":%u,"
        "\"t_render\":%.1f,\"t_pack\":%.1f,\"t_send\":%.1f,\"fps_eff\":%d,"
        "\"late\":%u,\"frames\":%u,"
        "\"beat_enabled\":%s,\"beat_offset\":%d,\"beat_pulse\":%d,"
        "\"beat_auto_bars\":%d,\"beat_auto_pool\":%d,\"beat_on_deck\":%s,"
        "\"beat_tempo_lock\":%s,\"beat_synced\":%s,\"beat_bpm\":%.2f,"
        "\"beat_deck\":%d,\"beat_age\":%ld,\"beat_err\":%.1f,\"beat_packets\":%u}",
        g_set.mode, g_set.effect, g_set.text_anim, g_set.brightness,
        g_set.speed, g_set.fps, g_set.font_scale, g_set.paused ? "true" : "false",
        g_set.playlist ? "true" : "false", g_set.playlist_secs,
        g_set.clock24 ? "true" : "false", g_set.clock_seconds ? "true" : "false",
        tz, tx,
        g_set.fg[0], g_set.fg[1], g_set.fg[2],
        g_set.bg[0], g_set.bg[1], g_set.bg[2], PANEL_W, PANEL_H,
        wifi_is_ap_mode() ? "true" : "false", wifi_ip_str(), wifi_ssid_str(),
        wifi_rssi(), time_is_valid() ? "true" : "false",
        (long long)(esp_timer_get_time() / 1000000),
        (unsigned)esp_get_free_heap_size(), render_fps_actual(),
        render_active() ? "true" : "false",
        g_tune.pclk_hz / 2e6, dp_qspi_get_clock() / 1e6, g_tune.burst,
        g_tune.gap1, g_tune.gap2, g_tune.pad,
        par ? scanout_par_refresh_hz() : 0.0f,
        scanout_get_backend(), scanout_running() ? "true" : "false",
        scanout_get_hold(), g_data_backend, dp_qspi_get_niborder() ? 1 : 0,
        g_proto.n_data_latches, g_proto.data_latch_code,
        g_proto.stop_gclk_in_preamble ? 1 : 0, g_proto.preamble_each_frame ? 1 : 0,
        g_map.mirror_x ? 1 : 0, g_map.flip_y ? 1 : 0, g_map.word_lsb_first ? 1 : 0,
        (unsigned)dp_get_halfperiod(),
        render_render_us() / 1000.0, render_pack_us() / 1000.0,
        render_send_us() / 1000.0, render_fps_effective(),
        (unsigned)g_proto_late_frames, (unsigned)g_proto_frames,
        g_beat.enabled ? "true" : "false", g_beat.offset_ms, g_beat.pulse,
        g_beat.auto_bars, g_beat.auto_pool, g_beat.on_deck ? "true" : "false",
        g_beat.tempo_lock ? "true" : "false", bs.synced ? "true" : "false", bs.bpm,
        bs.deck, (long)bs.age_ms, bs.err_ms, (unsigned)bs.packets);
}

// ----------------------------------------------------------------- handlers
static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    // Never cache the UI: a stale page keeps running its old polling loop
    // against new firmware, which is indistinguishable from a firmware bug.
    httpd_resp_set_hdr(r, "Cache-Control", "no-store, must-revalidate");
    return httpd_resp_send(r, web_index_html, web_index_html_len);
}

static esp_err_t h_state_get(httpd_req_t *r)
{
    return send_json(r, build_state(true));
}

static esp_err_t h_state_post(httpd_req_t *r)
{
    cJSON *j = read_json_body(r);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *v;
    bool touched = false;
    if ((v = cJSON_GetObjectItem(j, "mode")) && cJSON_IsNumber(v)) {
        if (g_set.mode != (uint8_t)v->valueint) touched = true;
        g_set.mode = (uint8_t)v->valueint;
    }
    if ((v = cJSON_GetObjectItem(j, "effect")) && cJSON_IsNumber(v)) {
        if (g_set.effect != (uint8_t)v->valueint) touched = true;
        g_set.effect = (uint8_t)v->valueint;
    }
    if ((v = cJSON_GetObjectItem(j, "text_anim")) && cJSON_IsNumber(v))
        g_set.text_anim = (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(j, "brightness")) && cJSON_IsNumber(v))
        g_set.brightness = (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(j, "speed")) && cJSON_IsNumber(v))
        g_set.speed = v->valueint < 1 ? 1 : (uint8_t)v->valueint;
    if ((v = cJSON_GetObjectItem(j, "fps")) && cJSON_IsNumber(v))
        g_set.fps = v->valueint < 5 ? 5 : (v->valueint > 240 ? 240 : (uint8_t)v->valueint);
    if ((v = cJSON_GetObjectItem(j, "font_scale")) && cJSON_IsNumber(v))
        g_set.font_scale = v->valueint < 1 ? 1 : (v->valueint > 4 ? 4 : (uint8_t)v->valueint);
    if ((v = cJSON_GetObjectItem(j, "paused")) && cJSON_IsBool(v))
        g_set.paused = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(j, "playlist")) && cJSON_IsBool(v))
        g_set.playlist = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(j, "playlist_secs")) && cJSON_IsNumber(v))
        g_set.playlist_secs = (uint16_t)(v->valueint < 3 ? 3 : v->valueint);
    if ((v = cJSON_GetObjectItem(j, "clock24")) && cJSON_IsBool(v))
        g_set.clock24 = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(j, "clock_seconds")) && cJSON_IsBool(v))
        g_set.clock_seconds = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(j, "tz")) && cJSON_IsString(v)) {
        strlcpy(g_set.tz, v->valuestring, sizeof(g_set.tz));
        time_sync_start();
    }
    if ((v = cJSON_GetObjectItem(j, "text")) && cJSON_IsString(v))
        strlcpy(g_set.text, v->valuestring, sizeof(g_set.text));
    if ((v = cJSON_GetObjectItem(j, "beat_enabled")) && cJSON_IsBool(v))
        g_beat.enabled = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(j, "beat_offset")) && cJSON_IsNumber(v))
        g_beat.offset_ms = (int16_t)(v->valueint < -500 ? -500 : (v->valueint > 500 ? 500 : v->valueint));
    if ((v = cJSON_GetObjectItem(j, "beat_pulse")) && cJSON_IsNumber(v))
        g_beat.pulse = (uint8_t)(v->valueint < 0 ? 0 : (v->valueint > 100 ? 100 : v->valueint));
    if ((v = cJSON_GetObjectItem(j, "beat_auto_bars")) && cJSON_IsNumber(v))
        g_beat.auto_bars = (uint8_t)(v->valueint < 0 ? 0 : (v->valueint > 64 ? 64 : v->valueint));
    if ((v = cJSON_GetObjectItem(j, "beat_auto_pool")) && cJSON_IsNumber(v))
        g_beat.auto_pool = v->valueint ? 1 : 0;
    if ((v = cJSON_GetObjectItem(j, "beat_on_deck")) && cJSON_IsBool(v))
        g_beat.on_deck = cJSON_IsTrue(v);
    if ((v = cJSON_GetObjectItem(j, "beat_tempo_lock")) && cJSON_IsBool(v))
        g_beat.tempo_lock = cJSON_IsTrue(v);
    rgb_from_json(cJSON_GetObjectItem(j, "fg"), g_set.fg);
    rgb_from_json(cJSON_GetObjectItem(j, "bg"), g_set.bg);
    cJSON_Delete(j);
    if (touched) render_touch();
    return send_json(r, build_state(false));
}

static esp_err_t h_action(httpd_req_t *r)
{
    cJSON *j = read_json_body(r);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *a = cJSON_GetObjectItem(j, "action");
    const char *act = cJSON_IsString(a) ? a->valuestring : "";
    if (!strcmp(act, "play"))       g_set.paused = false;
    else if (!strcmp(act, "pause")) g_set.paused = true;
    else if (!strcmp(act, "toggle"))g_set.paused = !g_set.paused;
    else if (!strcmp(act, "next"))  render_next_effect(1);
    else if (!strcmp(act, "prev"))  render_next_effect(-1);
    else if (!strcmp(act, "save"))  settings_save();
    else if (!strcmp(act, "blank")) g_set.mode = MODE_OFF;
    else if (!strcmp(act, "defaults")) { settings_defaults(); beat_cfg_defaults(); settings_save(); render_touch(); }
    else if (!strcmp(act, "reboot")) {
        cJSON_Delete(j);
        httpd_resp_sendstr(r, "{\"ok\":true}");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
        return ESP_OK;
    }
    cJSON_Delete(j);
    return send_json(r, build_state(false));
}

static esp_err_t h_wifi(httpd_req_t *r)
{
    cJSON *j = read_json_body(r);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *s = cJSON_GetObjectItem(j, "ssid"), *p = cJSON_GetObjectItem(j, "pass");
    cJSON *forget = cJSON_GetObjectItem(j, "forget");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"ok\":true}");
    if (cJSON_IsTrue(forget)) wifi_forget();
    else if (cJSON_IsString(s))
        wifi_set_credentials(s->valuestring, cJSON_IsString(p) ? p->valuestring : "");
    cJSON_Delete(j);
    return ESP_OK;
}

// Run any serial-console command and return its output (Advanced tab).
static esp_err_t h_cmd(httpd_req_t *r)
{
    cJSON *j = read_json_body(r);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *c = cJSON_GetObjectItem(j, "cmd");
    char *out = malloc(2048);
    if (!out) { cJSON_Delete(j); httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    out[0] = 0;
    if (cJSON_IsString(c)) console_exec(c->valuestring, out, 2048);
    cJSON_Delete(j);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "out", out);
    cJSON_AddBoolToObject(root, "ui", render_active());
    free(out);
    return send_json(r, root);
}

static esp_err_t h_scan(httpd_req_t *r)
{
    wifi_scan_config_t sc = { .show_hidden = false };
    cJSON *arr = cJSON_CreateArray();
    if (esp_wifi_scan_start(&sc, true) == ESP_OK) {
        uint16_t n = 12;
        wifi_ap_record_t recs[12];
        if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            for (int i = 0; i < n; i++) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "ssid", (char *)recs[i].ssid);
                cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
                cJSON_AddBoolToObject(o, "lock", recs[i].authmode != WIFI_AUTH_OPEN);
                cJSON_AddItemToArray(arr, o);
            }
        }
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "aps", arr);
    return send_json(r, root);
}

// ------------------------------------------------------------ sample texts
static esp_err_t h_samples_get(httpd_req_t *r)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", g_samples.enabled);
    cJSON *styles = cJSON_CreateArray();
    for (int i = 0; i < SMP_STYLE_COUNT; i++)
        cJSON_AddItemToArray(styles, cJSON_CreateString(g_sample_style_names[i]));
    cJSON_AddItemToObject(j, "styles", styles);
    cJSON *slots = cJSON_CreateArray();
    for (int i = 0; i < SAMPLE_SLOTS; i++) {
        const sample_slot_t *sl = &g_samples.slot[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "t", sl->text);
        cJSON_AddItemToObject(o, "c", rgb_to_json(sl->rgb));
        cJSON_AddNumberToObject(o, "s", sl->style);
        cJSON_AddItemToArray(slots, o);
    }
    cJSON_AddItemToObject(j, "slots", slots);
    return send_json(r, j);
}

// {"enabled": bool} and/or {"n": 1..32, "t": "SHOTS", "c": [r,g,b], "s": style}
static esp_err_t h_samples_post(httpd_req_t *r)
{
    cJSON *j = read_json_body(r);
    if (!j) { httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "bad json"); return ESP_FAIL; }
    cJSON *v;
    if ((v = cJSON_GetObjectItem(j, "enabled")) && cJSON_IsBool(v))
        g_samples.enabled = cJSON_IsTrue(v);
    cJSON *n = cJSON_GetObjectItem(j, "n");
    if (cJSON_IsNumber(n) && n->valueint >= 1 && n->valueint <= SAMPLE_SLOTS) {
        sample_slot_t *sl = &g_samples.slot[n->valueint - 1];
        if ((v = cJSON_GetObjectItem(j, "t")) && cJSON_IsString(v))
            strlcpy(sl->text, v->valuestring, sizeof(sl->text));
        rgb_from_json(cJSON_GetObjectItem(j, "c"), sl->rgb);
        if ((v = cJSON_GetObjectItem(j, "s")) && cJSON_IsNumber(v) &&
            v->valueint >= 0 && v->valueint < SMP_STYLE_COUNT)
            sl->style = (uint8_t)v->valueint;
    }
    if ((v = cJSON_GetObjectItem(j, "preview")) && cJSON_IsNumber(v))
        samples_preview(v->valueint);       // after any edit in the same request
    cJSON_Delete(j);
    httpd_resp_set_type(r, "application/json");
    return httpd_resp_sendstr(r, "{\"ok\":true}");
}

// Polled by the Samples tab while it's open (pad learning): tiny and cheap.
static esp_err_t h_samples_live(httpd_req_t *r)
{
    char b[96];
    uint64_t m = beatsync_samplers(NULL, NULL);
    snprintf(b, sizeof(b), "{\"mask\":\"%08lx%08lx\",\"last\":%d}",
             (unsigned long)(m >> 32), (unsigned long)(m & 0xffffffffu),
             beatsync_sampler_last());
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, b);
}

// One-shot snapshot of the panel as a binary PPM (200x30 RGB, pre-gamma).
// For checking effects remotely; nothing streams, so no WiFi load until asked.
static esp_err_t h_frame(httpd_req_t *r)
{
    const size_t n = (size_t)PANEL_W * PANEL_H * 3;
    uint8_t *buf = malloc(n);
    if (!buf) { httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "oom"); return ESP_FAIL; }
    render_snapshot(buf);
    char hdr[32];
    int hl = snprintf(hdr, sizeof(hdr), "P6\n%d %d\n255\n", PANEL_W, PANEL_H);
    httpd_resp_set_type(r, "image/x-portable-pixmap");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_send_chunk(r, hdr, hl);
    httpd_resp_send_chunk(r, (const char *)buf, n);
    free(buf);
    return httpd_resp_send_chunk(r, NULL, 0);
}

// ------------------------------------------------------------- ws preview
static esp_err_t h_ws(httpd_req_t *r)
{
    if (r->method == HTTP_GET) {          // handshake
        int fd = httpd_req_to_sockfd(r);
        for (int i = 0; i < MAX_WS_CLIENTS; i++)
            if (s_ws_fds[i] == 0) { s_ws_fds[i] = fd; break; }
        ESP_LOGI(TAG, "ws client %d connected", fd);
        return ESP_OK;
    }
    httpd_ws_frame_t f = { 0 };
    esp_err_t e = httpd_ws_recv_frame(r, &f, 0);
    if (e != ESP_OK) return e;
    if (f.len && f.len < 256) {
        uint8_t buf[256];
        f.payload = buf;
        httpd_ws_recv_frame(r, &f, f.len);
    }
    return ESP_OK;
}

static void ws_broadcast(const void *data, size_t len, httpd_ws_type_t type)
{
    httpd_ws_frame_t f = { .final = true, .type = type,
                           .payload = (uint8_t *)data, .len = len };
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!s_ws_fds[i]) continue;
        if (httpd_ws_send_frame_async(s_srv, s_ws_fds[i], &f) != ESP_OK) {
            ESP_LOGI(TAG, "ws client %d gone", s_ws_fds[i]);
            s_ws_fds[i] = 0;
        }
    }
}

// Pushes state to connected browsers. (There is deliberately no pixel preview:
// streaming the framebuffer was ~12 kB per frame of WiFi traffic, and every
// transmission couples into the panel's supply as a visible brightness dip.)
static void state_task(void *arg)
{
    static char sbuf[2048];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(STATE_PUSH_MS));
        if (!s_srv) continue;
        bool any = false;
        for (int i = 0; i < MAX_WS_CLIENTS; i++) if (s_ws_fds[i]) any = true;
        if (!any) continue;
        size_t n = state_json(sbuf, sizeof(sbuf));
        if (n < sizeof(sbuf)) ws_broadcast(sbuf, n, HTTPD_WS_TYPE_TEXT);
    }
}

void webserver_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 14;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.core_id = 0;              // keep the network side off the render core
    if (httpd_start(&s_srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return;
    }
    const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = h_root },
        { .uri = "/api/state",   .method = HTTP_GET,  .handler = h_state_get },
        { .uri = "/api/state",   .method = HTTP_POST, .handler = h_state_post },
        { .uri = "/api/action",  .method = HTTP_POST, .handler = h_action },
        { .uri = "/api/wifi",    .method = HTTP_POST, .handler = h_wifi },
        { .uri = "/api/scan",    .method = HTTP_GET,  .handler = h_scan },
        { .uri = "/api/cmd",     .method = HTTP_POST, .handler = h_cmd },
        { .uri = "/api/samples", .method = HTTP_GET,  .handler = h_samples_get },
        { .uri = "/api/samples", .method = HTTP_POST, .handler = h_samples_post },
        { .uri = "/api/samples/live", .method = HTTP_GET, .handler = h_samples_live },
        { .uri = "/api/frame",   .method = HTTP_GET,  .handler = h_frame },
        { .uri = "/ws", .method = HTTP_GET, .handler = h_ws, .is_websocket = true },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++)
        httpd_register_uri_handler(s_srv, &uris[i]);
    xTaskCreatePinnedToCore(state_task, "state", 4096, NULL, 3, NULL, 0);  // core 0
    ESP_LOGI(TAG, "web UI ready at http://%s/", wifi_ip_str());
}

void webserver_stop(void)
{
    if (s_srv) { httpd_stop(s_srv); s_srv = NULL; }
}
