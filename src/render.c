#include "render.h"
#include <string.h>
#include "settings.h"
#include "canvas.h"
#include "effects.h"
#include "beatsync.h"
#include "samples.h"
#include <math.h>
#include "framebuf.h"
#include "protocol.h"
#include "scanout.h"
#include "scanout_par.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"

#define TAG "render"

// Two-stage pipeline across the two cores:
//
//   core 0  content task  — runs the effect/text/clock into the draw canvas,
//                           then publishes it. No deadline; may be preempted
//                           by WiFi/lwIP/HTTP freely.
//   core 1  display task  — packs the published canvas into driver slots and
//                           emits the frame + vsync on a rigid cycle grid.
//
// Frame emission is what must be regular: every vsync briefly stops the
// grayscale clock, so a varying interval modulates brightness (pulsing) and a
// late frame shows the previous buffer (judder). Keeping content generation
// off that core means an expensive effect — or a WiFi burst — can never delay
// a vsync; at worst the previous frame is displayed again.

static TaskHandle_t s_content, s_display;
static SemaphoreHandle_t s_pub_mux;      // guards the published canvas
static volatile bool s_run;
static volatile bool s_dirty = true;     // effect changed: reset fx state
static volatile bool s_content_new;      // a fresh frame has been published
static uint32_t s_frame;
static float s_fps_actual;
static uint32_t s_last_hash;
static uint32_t s_static;                // consecutive unchanged frames
static uint32_t s_render_us, s_pack_us, s_hash_us;
static int s_fps_effective = 30;

uint32_t render_frame_count(void) { return s_frame; }
uint32_t render_canvas_hash(void) { return s_last_hash; }
int render_fps_effective(void) { return s_fps_effective; }
float render_fps_actual(void) { return s_fps_actual; }
bool render_active(void) { return s_run; }
uint32_t render_render_us(void) { return s_render_us; }
uint32_t render_pack_us(void)   { return s_pack_us + s_hash_us; }
uint32_t render_send_us(void)   { return g_proto_send_us; }
void render_touch(void) { s_dirty = true; s_last_hash = 0; s_static = 0; }

// Animation speed multiplier: the speed slider (50 = 1x), times the track's
// tempo relative to 120 BPM when "tempo lock" is on and Mixxx is connected.
float render_speed(void)
{
    float k = (g_set.speed ? g_set.speed : 1) / 50.0f;
    if (g_beat.tempo_lock && g_bf.synced && g_bf.bpm > 0) k *= g_bf.bpm / 120.0f;
    return k;
}

void render_next_effect(int delta)
{
    int n = g_effect_count;
    g_set.effect = (uint8_t)(((int)g_set.effect + delta % n + n) % n);
    s_dirty = true;
}

static int effective_fps(void)
{
    // Never promise a rate the pipeline can't hold: a missed slot rebases the
    // vsync pacing, which is exactly what pulsing and judder are. Budget 65%
    // of the slot for the display side; the rest absorbs jitter.
    int fps = g_set.fps ? g_set.fps : 30;
    uint32_t work = s_pack_us + g_proto_send_us;
    if (work > 1000) {
        int cap = (int)(650000u / work);
        if (cap < 5) cap = 5;
        if (cap < fps) fps = cap;
    }
    s_fps_effective = fps;
    return fps;
}

// ------------------------------------------------------------ core 0
static void content_task(void *arg)
{
    // Animation clock in microseconds. The S3 version accumulated whole
    // milliseconds per frame, which truncated 12.8 ms frames to 12 or 13 ms
    // (and integer-divided again for the speed scale) -- uneven steps for
    // every time-based effect. Keep full precision; effects still get ms.
    uint64_t anim_us = 0;
    uint32_t anim = 0;
    int64_t last = esp_timer_get_time();
    int64_t last_switch = last;
    int64_t last_group = 0;         // bar group for auto-change
    bool have_group = false;
    uint32_t last_deck_changes = 0;

    for (;;) {
        // One frame produced per frame consumed (the display task kicks us),
        // with a timeout so we keep running if the display side is idle.
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
        if (!s_run) { last = esp_timer_get_time(); continue; }

        int64_t now = esp_timer_get_time();
        int64_t dt_us = now - last;
        if (dt_us > 200000) dt_us = 200000;
        last = now;

        if (s_dirty) { fx_reset(); s_dirty = false; }
        if (!g_set.paused)
            anim_us += (uint64_t)((float)dt_us * render_speed());
        anim = (uint32_t)(anim_us / 1000);

        // Beat position for this frame. The frame rendered now is shown about
        // two frame periods later (it is packed after the current frame's
        // send + vsync, then displayed until the next vsync), so look ahead.
        int fps = s_fps_effective > 0 ? s_fps_effective : 30;
        beatsync_frame(now + 2 * (1000000 / fps), anim_us, &g_bf);

        // DJ automation: new animation every N bars / when the on-air deck
        // changes. Switching here, before rendering, puts the change exactly
        // on the bar line.
        beat_status_t bs;
        beatsync_status(&bs);
        if (g_set.mode == MODE_EFFECT && g_bf.synced) {
            bool sw = false;
            if (g_beat.auto_bars) {
                int64_t grp = (int64_t)floor(g_bf.beats / (4.0 * g_beat.auto_bars));
                if (have_group && grp != last_group) sw = true;
                last_group = grp;
                have_group = true;
            }
            if (g_beat.on_deck && bs.deck_changes != last_deck_changes) sw = true;
            if (sw) {
                g_set.effect = (uint8_t)fx_next(g_set.effect, 1, g_beat.auto_pool == 0);
                fx_reset();
                s_dirty = false;
            }
        } else {
            have_group = false;
        }
        last_deck_changes = bs.deck_changes;

        if (g_set.playlist && g_set.mode == MODE_EFFECT && !g_set.paused &&
            (now - last_switch) > (int64_t)g_set.playlist_secs * 1000000) {
            last_switch = now;
            render_next_effect(1);
            fx_reset();
        }

        int64_t r0 = esp_timer_get_time();
        // A sample text (Mixxx sampler playing) takes over until it ends.
        uint32_t smp_ms = 0;
        const sample_slot_t *smp = g_set.mode != MODE_OFF ? samples_active(NULL, &smp_ms) : NULL;
        if (smp) samples_render(smp, anim, smp_ms);
        else switch (g_set.mode) {
        case MODE_TEXT:  textfx_render(g_set.text_anim, g_set.text, anim, s_frame); break;
        case MODE_CLOCK: clock_render(anim, s_frame); break;
        case MODE_OFF:   canvas_clear(); break;
        default:         fx_render(g_set.effect, anim, s_frame); break;
        }
        s_render_us = (uint32_t)(esp_timer_get_time() - r0);

        int64_t h0 = esp_timer_get_time();
        uint32_t h = canvas_hash() ^ ((uint32_t)g_set.brightness << 8)
                                   ^ ((uint32_t)g_set.mode << 20);
        s_hash_us = (uint32_t)(esp_timer_get_time() - h0);

        // Tracked for the web preview (which does skip unchanged frames) —
        // the panel itself is refreshed on a fixed cadence regardless.
        if (h != s_last_hash) { s_last_hash = h; s_static = 0; }
        else if (s_static < 0xffff) s_static++;

        // Beat pulse: brightness of whatever is showing dips between beats.
        uint16_t pulse_m = 256;
        if (g_beat.pulse && g_bf.synced && g_set.mode != MODE_OFF) {
            float fr = (float)(g_bf.beats - floor(g_bf.beats));
            float depth = g_beat.pulse / 100.0f;
            float f = 1.0f - depth * (1.0f - expf(-4.0f * fr));
            pulse_m = (uint16_t)(f * 256.0f + 0.5f);
        }

        xSemaphoreTake(s_pub_mux, portMAX_DELAY);
        if (pulse_m < 256) canvas_publish_scaled(pulse_m);
        else canvas_publish();
        s_content_new = true;
        xSemaphoreGive(s_pub_mux);
    }
}

// ------------------------------------------------------------ core 1
static void display_task(void *arg)
{
    uint32_t pace = 0;
    int64_t fps_t0 = esp_timer_get_time();
    uint32_t fps_f0 = 0;

    for (;;) {
        if (!s_run) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        int fps = effective_fps();

        // Emit every slot unconditionally, even when the image is unchanged.
        // Stopping while static and restarting on the next change is one
        // irregular vsync interval each way — which is one visible pulse. A
        // scrolling message hits it every loop, when the text is briefly off
        // screen and the canvas stops changing. Constant cadence costs a few
        // ms of otherwise idle core-1 time and removes the transition
        // entirely; the OEM controller likewise ran flat out regardless of
        // content.
        int64_t p0 = esp_timer_get_time();
        xSemaphoreTake(s_pub_mux, portMAX_DELAY);
        canvas_pack_slots(g_set.mode == MODE_OFF ? 0 : g_set.brightness);
        s_content_new = false;
        xSemaphoreGive(s_pub_mux);
        s_pack_us = (uint32_t)(esp_timer_get_time() - p0);

        // Kick the producer now: it renders the next frame on core 0 while
        // this core is busy shifting the current one out.
        xTaskNotifyGive(s_content);

        proto_send_frame_paced(&pace, fps);
        s_frame++;

        int64_t now = esp_timer_get_time();
        if (now - fps_t0 > 1000000) {
            s_fps_actual = (s_frame - fps_f0) * 1000000.0f / (now - fps_t0);
            fps_t0 = now; fps_f0 = s_frame;
        }
    }
}

void render_init(void)
{
    s_pub_mux = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(content_task, "content", 6144, NULL, 5, &s_content, 0);
    xTaskCreatePinnedToCore(display_task, "display", 4096, NULL,
                            PROTO_SENDER_PRIO, &s_display, 1);
    ESP_LOGI(TAG, "pipeline up: content on core 0, display on core 1 "
                  "(%d effects, %d text animations)",
             g_effect_count, g_textanim_count);
}

void render_start(void)
{
    proto_auto_stop();       // the render pipeline owns the panel
    s_dirty = true;
    s_run = true;
    if (s_content) xTaskNotifyGive(s_content);
}

void render_stop(void) { s_run = false; }

// Copy of the frame currently on the panel (200x30 RGB888, before gamma),
// for the web API's snapshot. Holds the publish lock for one memcpy.
void render_snapshot(uint8_t *rgb)
{
    xSemaphoreTake(s_pub_mux, portMAX_DELAY);
    memcpy(rgb, g_canvas_show, sizeof(g_canvas_show));
    xSemaphoreGive(s_pub_mux);
}
