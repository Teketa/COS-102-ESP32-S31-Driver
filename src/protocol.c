#include "protocol.h"
#include "datapath.h"
#include "datapath_qspi.h"
#include "scanout.h"
#include "scanout_par.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

proto_cfg_t g_proto = {
    // Hardware findings 2026-07-26: vsync resets the SRAM write pointer;
    // config-write commands advance it. Steady state therefore runs
    // vsync-only frames with all 128 latches; config is written once at boot.
    // (The OEM's per-frame config rewrite is why it sends only 127.)
    .n_data_latches = SRAM_SLOTS,
    .data_latch_code = LCODE_DATA,
    .preamble_each_frame = false,
    .stop_gclk_in_preamble = true,   // required: chaos with GCLK running
    .cfg_a = CFG_A_VALUE,
    .cfg_b_r = CFG_B_R, .cfg_b_g = CFG_B_G, .cfg_b_b = CFG_B_B,
};

slot_t g_slots[SRAM_SLOTS];

// The panel bus has several users (render task, console, web API). Everything
// that drives CLK/LAT/data or reconfigures a backend must hold this.
static SemaphoreHandle_t s_lock;

void proto_lock(void)   { if (s_lock) xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
void proto_unlock(void) { if (s_lock) xSemaphoreGiveRecursive(s_lock); }

static TaskHandle_t s_auto_task = NULL;
static volatile int s_auto_fps = 0;

void proto_init(void)
{
    memset(g_slots, 0, sizeof(g_slots));
    s_lock = xSemaphoreCreateRecursiveMutex();
}

void proto_boot(void)
{
    // One-time init: capture5's code-4 write plus the full config set that
    // the OEM re-sends per frame (registers are sticky — once is enough).
    proto_lock();
    bool was_running = scanout_running();
    scanout_stop();
    vTaskDelay(pdMS_TO_TICKS(2));
    dp_config_write(g_proto.cfg_a, g_proto.cfg_a, g_proto.cfg_a, LCODE_CFG_BOOT);
    dp_latch_only(LCODE_PREACTIVE);
    dp_config_write(g_proto.cfg_a, g_proto.cfg_a, g_proto.cfg_a, LCODE_CFG_A);
    dp_latch_only(LCODE_PREACTIVE);
    dp_config_write(g_proto.cfg_b_r, g_proto.cfg_b_g, g_proto.cfg_b_b, LCODE_CFG_B);
    // Align the write pointer once so the first data-then-vsync frame is
    // consistent (config writes above advanced it).
    dp_latch_only(LCODE_VSYNC);
    if (was_running) scanout_start();
    proto_unlock();
}

void proto_vsync_only(void)
{
    proto_lock();
    if (g_proto.stop_gclk_in_preamble) scanout_pause();
    dp_latch_only(LCODE_VSYNC);
    if (g_proto.stop_gclk_in_preamble) scanout_resume();
    proto_unlock();
}

void proto_preamble(void)
{
    // OEM per-frame preamble (capture4/12):
    //   vsync(3) -> preactive(14) -> [0x03EA x50]+code10
    //            -> preactive(14) -> [gains x50]+code8
    // all inside a ~174us GCLK-quiet window.
    proto_lock();
    if (g_proto.stop_gclk_in_preamble) scanout_pause();
    dp_latch_only(LCODE_VSYNC);
    dp_latch_only(LCODE_PREACTIVE);
    dp_config_write(g_proto.cfg_a, g_proto.cfg_a, g_proto.cfg_a, LCODE_CFG_A);
    dp_latch_only(LCODE_PREACTIVE);
    dp_config_write(g_proto.cfg_b_r, g_proto.cfg_b_g, g_proto.cfg_b_b, LCODE_CFG_B);
    if (g_proto.stop_gclk_in_preamble) scanout_resume();
    proto_unlock();
}

int g_data_backend = DATA_BACKEND_BITBANG;
uint32_t g_proto_send_us;      // last data-send duration (excl. pacing wait)
uint32_t g_proto_late_frames;  // vsyncs that missed the nominal cycle grid
uint32_t g_proto_frames;       // vsyncs emitted

void proto_send_slots(int first, int count)
{
    proto_lock();
    if (g_data_backend == DATA_BACKEND_QSPI) {
        dp_qspi_begin();
        for (int i = 0; i < count; i++) {
            const slot_t *s = &g_slots[(first + i) % SRAM_SLOTS];
            dp_qspi_send_slot(s->r, s->g, s->b);
        }
        dp_qspi_end();
    } else {
        for (int i = 0; i < count; i++) {
            const slot_t *s = &g_slots[(first + i) % SRAM_SLOTS];
            dp_shift_latch(s->r, s->g, s->b, WORDS_PER_SHIFT, g_proto.data_latch_code);
        }
    }
    proto_unlock();
}

void proto_send_frame(void)
{
    // Data first, then vsync: the drivers are double-buffered and vsync swaps
    // in whatever was written before it. (Vsync-first shows the previous
    // frame's data — one command behind, as observed on hardware.)
    proto_send_slots(0, g_proto.n_data_latches);
    if (g_proto.preamble_each_frame) proto_preamble();
    else proto_vsync_only();
}

// Send one frame, paced to `fps`. `next_cycle` is caller-owned pacing state
// (init to 0). Shared by the console `auto` streamer and the render engine.
void proto_send_frame_paced(uint32_t *next_cycle_p, int fps)
{
    uint32_t next_cycle = *next_cycle_p;
    proto_lock();
    {
        if (fps <= 0) fps = 1;
        if (scanout_get_backend() == SCAN_BACKEND_PARLIO && scanout_running()) {
            // Exact-cycle frame pacing: send data while scanning, then stop
            // the scan at a precise cycle count for the vsync. A constant
            // GCLK-pulse count between vsyncs keeps the drivers' S-PWM
            // truncation constant -> no brightness pulsing (the OEM does the
            // same: exactly 194 bursts/frame, +3 pulses after the preamble).
            uint32_t cyc_per_frame = 1000000u / fps / scanout_par_cycle_us();
            if (cyc_per_frame < 2) cyc_per_frame = 2;
            if (next_cycle == 0) next_cycle = scanout_par_cycle_count();
            next_cycle += cyc_per_frame;
            int64_t t0 = esp_timer_get_time();
            proto_send_slots(0, g_proto.n_data_latches);
            g_proto_send_us = (uint32_t)(esp_timer_get_time() - t0);
            scanout_par_pause_at(next_cycle);
            // Send the vsync directly — no nested pause/resume: the display
            // is dark here and the paused phase's column FETs stay selected,
            // so every microsecond of dark time glows on those columns. Keep
            // it minimal and constant (OEM: fixed ~174us window). Run at top
            // priority so WiFi/HTTP tasks can't stretch the window and
            // modulate brightness.
            // Restore to a fixed base priority, not uxTaskPriorityGet(): that
            // can return a temporarily inherited priority and make the boost
            // permanent, which would starve the scan feeder for good.
            vTaskPrioritySet(NULL, configMAX_PRIORITIES - 1);
            dp_latch_only(LCODE_VSYNC);
            if (g_proto.preamble_each_frame) {
                dp_latch_only(LCODE_PREACTIVE);
                dp_config_write(g_proto.cfg_a, g_proto.cfg_a, g_proto.cfg_a, LCODE_CFG_A);
                dp_latch_only(LCODE_PREACTIVE);
                dp_config_write(g_proto.cfg_b_r, g_proto.cfg_b_g, g_proto.cfg_b_b, LCODE_CFG_B);
            }
            scanout_par_resume();
            vTaskPrioritySet(NULL, PROTO_SENDER_PRIO);
            // Diagnostic: did this vsync land on the nominal grid? A frame
            // that has to rebase got a longer GCLK interval than its
            // neighbours, and since the drivers' PWM freezes while GCLK is
            // stopped, that is exactly a visible brightness pulse.
            g_proto_frames++;
            if ((int32_t)(scanout_par_cycle_count() - next_cycle) > 0) {
                g_proto_late_frames++;
                next_cycle = scanout_par_cycle_count();  // ran late; rebase
            }
        } else {
            next_cycle = 0;
            int64_t period_us = 1000000 / fps;
            int64_t t0 = esp_timer_get_time();
            proto_send_frame();
            int64_t dt = esp_timer_get_time() - t0;
            vTaskDelay(pdMS_TO_TICKS(dt < period_us ? (period_us - dt) / 1000 + 1 : 1));
        }
    }
    proto_unlock();
    *next_cycle_p = next_cycle;
}

static void auto_task(void *arg)
{
    uint32_t pace = 0;
    while (s_auto_fps > 0) proto_send_frame_paced(&pace, s_auto_fps);
    s_auto_task = NULL;
    vTaskDelete(NULL);
}

void proto_auto_start(int fps)
{
    if (s_auto_task) { s_auto_fps = fps; return; }
    s_auto_fps = fps;
    xTaskCreatePinnedToCore(auto_task, "framestream", 4096, NULL, 5, &s_auto_task, 0);
}

void proto_auto_stop(void)
{
    s_auto_fps = 0;
    while (s_auto_task) vTaskDelay(1);
}

bool proto_auto_running(void) { return s_auto_task != NULL; }
