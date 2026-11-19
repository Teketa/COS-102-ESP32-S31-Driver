// Parallel IO (PARLIO) TX DMA scan backend: replays one full scan cycle from
// a DMA buffer, continuously, with zero CPU per cycle.
//
// This is the S31 port of the S3's LCD_CAM engine (scanout_lcd.c there). The
// S31 has LCD_CAM too, but its i80 bus is 8 bits wide and needs WR, DC and
// four unused data lines on real GPIOs -- six pins the hat doesn't have.
// PARLIO TX does the same job on exactly the four lines the hat wires:
//
//   data bit 0 = GCLK   (PIN_GCLK)
//   data bit 1 = ADDR A (PIN_ADDR_A)
//   data bit 2 = ADDR B (PIN_ADDR_B)
//   data bit 3 = ADDR C (PIN_ADDR_C)
//
// 4-bit bus, two samples per byte. Buffer layout per scan cycle (pclk 40 MHz
// -> GCLK 20 MHz):
//   for each phase 0..7:
//     GAP1 samples GCLK low (address settles)      ~1.0 us
//     2*1041 samples GCLK toggling (1041 pulses)   ~52 us
//     GAP2 samples GCLK low                        ~0.75 us
//   tail: address back to 0, GCLK low
// A feeder task keeps the PARLIO transaction queue full; pausing stops feeding
// and drains the queue, so the pause lands exactly on a cycle boundary (the
// alignment requirement discovered on hardware).
#include "scanout_par.h"
#include "pins.h"
#include "replay_data.h"
#include <string.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/parlio_tx.h"
#include "esp_clk_tree.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

#define TAG "scan_par"
// GCLK = pclk / 2. Default 20 MHz GCLK (measured stable on this panel with
// the S3 controller; the OEM used 12.5 MHz, the drivers are rated 30 MHz).
#define PCLK_HZ      40000000
#define GAP1_SAMPLES 40   // addr changes at segment start (OEM: ~1.75 us lead-in)
#define GAP2_SAMPLES 30   // tail after each burst (OEM: ~1.27 us)
// OEM pads the first burst after the vsync quiet with +3 pulses (their
// measured 1044-burst) -- compensates phase 0's lost GCLK share.
#define FIRST_BURST_PAD 3
// The cycle ends with the address already back at 0 and GCLK low, so a pause
// (i.e. every vsync) rests at addr 0 -- matching the OEM exactly: captures
// 17/18/19 all show addr stepping 7 -> 0 *before* the vsync and holding 0
// across the whole preamble. Resting at 7 instead leaves the panel in a
// different scan state at frame swap and displaces rows.
// (Minimum; rounded up so every cycle is a whole number of 32-bit words.)
#define TAIL_SAMPLES 20
// Two keeps GCLK gapless (one running + one queued) and keeps the pause from
// overshooting the target cycle when a frame runs late.
#define QUEUE_DEPTH  2
#define DATA_WIDTH   4

#define BIT_GCLK 0x01
#define BIT_A    0x02
#define BIT_B    0x04
#define BIT_C    0x08

static parlio_tx_unit_handle_t s_unit;
static uint8_t *s_buf;                     // normal cycle
static uint8_t *s_buf_first;               // post-vsync variant (+pad GCLK in phase 0)
static size_t s_samples, s_samples_first;
static volatile bool s_first_pending;      // submit s_buf_first on next cycle
static TaskHandle_t s_task;
static volatile bool s_run, s_pause_req;
static volatile uint32_t s_submitted, s_done;
static volatile uint32_t s_limit;          // stop submitting at this cycle (0=off)
static volatile TaskHandle_t s_waiter;     // task blocked in pause_at
static SemaphoreHandle_t s_mux;            // serializes submit-reservation vs pause
static uint32_t s_cycle_us_meas;           // measured cycle period (0 = not yet)
static int64_t s_meas_t0;
static uint32_t s_meas_c0;

static bool IRAM_ATTR on_done(parlio_tx_unit_handle_t unit,
                              const parlio_tx_done_event_data_t *ed, void *ctx)
{
    s_done++;
    BaseType_t hpw = pdFALSE;
    // Wake the pauser only when every queued cycle has finished -- GCLK must
    // be idle before the vsync goes out.
    if (s_waiter && s_done == s_submitted) {
        vTaskNotifyGiveFromISR((TaskHandle_t)s_waiter, &hpw);
        s_waiter = NULL;
    }
    return hpw == pdTRUE;
}

// Runtime-tunable timing (overclocking). Defaults = S3-proven values.
scan_tuning_t g_tune = {
    .pclk_hz = PCLK_HZ,
    .burst = OEM_GCLK_BURST,
    .gap1 = GAP1_SAMPLES,
    .gap2 = GAP2_SAMPLES,
    .pad = FIRST_BURST_PAD,
};

static uint32_t src_clk_hz(void)
{
    uint32_t hz = 0;
    esp_clk_tree_src_get_freq_hz((soc_module_clk_t)PARLIO_CLK_SRC_DEFAULT,
                                 ESP_CLK_TREE_SRC_FREQ_PRECISION_CACHED, &hz);
    return hz ? hz : 160000000;
}

uint32_t scanout_par_quantize_pclk(uint32_t hz)
{
    if (!hz) return 0;
    uint32_t src = src_clk_hz();
    uint32_t div = (src + hz / 2) / hz;
    if (div < 2) div = 2;                  // pclk <= 80 MHz (GCLK <= 40 MHz)
    return src / div;
}

// ------------------------------------------------------------ buffer build
static size_t cycle_samples(bool first)
{
    size_t n = (first ? 2 * g_tune.pad : 0) +
               8 * (g_tune.gap1 + 2 * g_tune.burst + g_tune.gap2) + TAIL_SAMPLES;
    return (n + 7) & ~(size_t)7;           // whole 32-bit words (8 samples)
}

// PARLIO_BIT_PACK_ORDER_LSB: the first sample of each byte is the low nibble.
// (If that were ever the wrong way round, GCLK would still toggle and the
// address would just change one sample later -- harmless.)
static inline void put(uint8_t *buf, size_t *n, uint8_t s)
{
    size_t i = *n >> 1;
    if (*n & 1) buf[i] |= (uint8_t)(s << 4);
    else        buf[i] = s & 0x0F;
    (*n)++;
}

static void fill_cycle(uint8_t *buf, size_t total, bool first)
{
    size_t n = 0;
    if (first)
        for (int i = 0; i < g_tune.pad; i++) { put(buf, &n, BIT_GCLK); put(buf, &n, 0); }
    for (int phase = 0; phase < 8; phase++) {
        uint8_t addr = ((phase & 1) ? BIT_A : 0) | ((phase & 2) ? BIT_B : 0) |
                       ((phase & 4) ? BIT_C : 0);
        for (int i = 0; i < g_tune.gap1; i++) put(buf, &n, addr);
        for (int i = 0; i < g_tune.burst; i++) {
            put(buf, &n, addr | BIT_GCLK);
            put(buf, &n, addr);
        }
        for (int i = 0; i < g_tune.gap2; i++) put(buf, &n, addr);
    }
    while (n < total) put(buf, &n, 0);     // rest at addr 0, GCLK low
}

static uint8_t *alloc_cycle(size_t samples)
{
    size_t bytes = ((samples / 2) + 63) & ~(size_t)63;
    return heap_caps_aligned_calloc(64, 1, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
}

static bool build_buffers(void)
{
    heap_caps_free(s_buf);       s_buf = NULL;
    heap_caps_free(s_buf_first); s_buf_first = NULL;
    s_samples = cycle_samples(false);
    s_samples_first = cycle_samples(true);
    s_buf = alloc_cycle(s_samples);
    s_buf_first = alloc_cycle(s_samples_first);
    if (!s_buf || !s_buf_first) {
        ESP_LOGE(TAG, "cycle buffer alloc failed (%u + %u B)",
                 (unsigned)(s_samples / 2), (unsigned)(s_samples_first / 2));
        return false;
    }
    fill_cycle(s_buf, s_samples, false);
    fill_cycle(s_buf_first, s_samples_first, true);
    s_cycle_us_meas = 0;
    s_meas_t0 = 0;
    return true;
}

// ------------------------------------------------------------ feeder
static void feeder_task(void *arg)
{
    const parlio_transmit_config_t tx_cfg = { .idle_value = 0 };
    for (;;) {
        // Reserve the cycle under the mutex *before* queuing it, so a
        // concurrent pause_at() can never miss an in-flight transfer.
        xSemaphoreTake(s_mux, portMAX_DELAY);
        bool go = s_unit && s_run && !s_pause_req &&
                  !(s_limit && (int32_t)(s_submitted - s_limit) >= 0);
        bool first = false;
        if (go) { s_submitted++; first = s_first_pending; s_first_pending = false; }
        xSemaphoreGive(s_mux);
        if (!go) {
            // woken immediately by resume/start; 1-tick timeout as fallback
            ulTaskNotifyTake(pdTRUE, 1);
            continue;
        }
        // Blocks while the transaction queue is full; the driver starts the
        // next queued transfer from its ISR, so cycles run back to back.
        const uint8_t *buf = first ? s_buf_first : s_buf;
        size_t samples = first ? s_samples_first : s_samples;
        esp_err_t err = parlio_tx_unit_transmit(s_unit, buf, samples * DATA_WIDTH, &tx_cfg);
        if (err != ESP_OK) { s_submitted--; vTaskDelay(1); }
    }
}

// ------------------------------------------------------------ init / tuning
bool scanout_par_init(void)
{
    if (!s_mux) s_mux = xSemaphoreCreateMutex();
    if (s_unit) return true;
    g_tune.pclk_hz = scanout_par_quantize_pclk(g_tune.pclk_hz);
    if (!build_buffers()) return false;

    parlio_tx_unit_config_t cfg = {
        .clk_src = PARLIO_CLK_SRC_DEFAULT,
        .clk_in_gpio_num = -1,
        .output_clk_freq_hz = g_tune.pclk_hz,
        .data_width = DATA_WIDTH,
        .clk_out_gpio_num = -1,              // the panel has no use for it
        .valid_gpio_num = -1,
        .trans_queue_depth = QUEUE_DEPTH,
        .max_transfer_size = s_samples_first / 2,
        // Big bursts: the stream runs continuously and must not lose SRAM
        // bandwidth to WiFi DMA -- a starved FIFO stalls the clock, and a
        // stalled GCLK freezes the drivers' PWM counter (a visible pulse).
        .dma_burst_size = 32,
        .shift_edge = PARLIO_SHIFT_EDGE_POS,
        .bit_pack_order = PARLIO_BIT_PACK_ORDER_LSB,
    };
    for (int i = 0; i < PARLIO_TX_UNIT_MAX_DATA_WIDTH; i++) cfg.data_gpio_nums[i] = -1;
    cfg.data_gpio_nums[0] = PIN_GCLK;
    cfg.data_gpio_nums[1] = PIN_ADDR_A;
    cfg.data_gpio_nums[2] = PIN_ADDR_B;
    cfg.data_gpio_nums[3] = PIN_ADDR_C;

    esp_err_t err = parlio_new_tx_unit(&cfg, &s_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PARLIO TX init failed: %s", esp_err_to_name(err));
        s_unit = NULL;
        return false;
    }
    const parlio_tx_event_callbacks_t cbs = { .on_trans_done = on_done };
    parlio_tx_unit_register_event_callbacks(s_unit, &cbs, NULL);
    err = parlio_tx_unit_enable(s_unit);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PARLIO TX enable failed: %s", esp_err_to_name(err));
        parlio_del_tx_unit(s_unit);
        s_unit = NULL;
        return false;
    }
    if (!s_task)
        xTaskCreatePinnedToCore(feeder_task, "scan_par", 3072, NULL, 10, &s_task, 1);
    ESP_LOGI(TAG, "PARLIO scanout: GCLK %.2f MHz, burst %d, refresh %.0f Hz, %u B/cycle",
             g_tune.pclk_hz / 2e6, g_tune.burst, scanout_par_refresh_hz(),
             (unsigned)(s_samples / 2));
    return true;
}

static void teardown(void)
{
    if (!s_unit) return;
    parlio_tx_unit_wait_all_done(s_unit, 100);
    parlio_tx_unit_disable(s_unit);
    parlio_del_tx_unit(s_unit);
    s_unit = NULL;
}

// Reconfigure timing at runtime (overclocking). Tears down and rebuilds the
// PARLIO unit, so the scan briefly stops. Returns false and restores the
// previous configuration if the new one can't be applied.
bool scanout_par_set_tuning(const scan_tuning_t *t)
{
    scan_tuning_t old = g_tune;
    bool was_running = s_run;
    if (t->burst < 16 || t->burst > 4096 || t->gap1 < 1 || t->gap2 < 1 ||
        t->pad < 0 || t->pad > 64 || t->pclk_hz < 1000000 || t->pclk_hz > 80000000)
        return false;

    scanout_par_stop();
    teardown();
    g_tune = *t;
    if (!scanout_par_init()) {                 // roll back
        g_tune = old;
        teardown();
        scanout_par_init();
        if (was_running) scanout_par_start();
        return false;
    }
    if (was_running) scanout_par_start();
    return true;
}

// ------------------------------------------------------------ run control
static uint32_t nominal_cycle_us(void)
{
    if (!g_tune.pclk_hz || !s_samples) return 1;
    uint32_t us = (uint32_t)((uint64_t)s_samples * 1000000 / g_tune.pclk_hz);
    return us ? us : 1;
}

static uint32_t measured_cycle_us(void);

// Panel refresh rate: full 8-phase scan cycles per second (measured).
float scanout_par_refresh_hz(void)
{
    return 1e6f / (float)measured_cycle_us();
}

void scanout_par_start(void)
{
    s_pause_req = false;
    s_run = true;
    if (s_task) xTaskNotifyGive(s_task);
}

void scanout_par_stop(void) { s_run = false; scanout_par_pause_wait(); }

void scanout_par_pause_wait(void)
{
    s_pause_req = true;
    // wait for all queued cycles to finish -> GCLK idle low, cycle boundary
    while (s_done != s_submitted) vTaskDelay(1);
    s_limit = s_done;
}

void scanout_par_resume(void)
{
    s_first_pending = true;   // first post-vsync cycle gets the padded burst
    s_limit = 0;
    s_pause_req = false;
    if (s_task) xTaskNotifyGive(s_task);   // restart GCLK with minimal latency
}

bool scanout_par_running(void) { return s_unit && s_run && !s_pause_req; }

// Schedule the scan to stop exactly after `cycle` completed cycles and block
// until it has. The feeder stops submitting just in time, so GCLK dark time
// is only the vsync itself (the OEM's ~174 us quiet), not a queue drain.
void scanout_par_pause_at(uint32_t cycle)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    if ((int32_t)(cycle - s_submitted) < 0) cycle = s_submitted;
    s_limit = cycle;                       // feeder stops reserving new cycles
    bool idle = (s_done == s_submitted);
    if (!idle) s_waiter = xTaskGetCurrentTaskHandle();
    xSemaphoreGive(s_mux);
    if (idle) return;
    // Wait for *every* queued cycle to finish, not just the target count: the
    // feeder runs ahead by up to the queue depth, and a vsync sent while those
    // are still clocking lands mid-scan-cycle -> random row alignment.
    while (s_done != s_submitted) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50));
    s_waiter = NULL;
}

uint32_t scanout_par_cycle_count(void) { return s_done; }

// Cycle period used for frame pacing: the buffer length / pclk, a constant.
// It must NOT be a live measurement: frame pacing turns it into a whole number
// of scan cycles per frame, and when fps sits near a boundary (78 fps = 29.0
// cycles) a measurement drifting by 1 us flips that count between 28 and 29.
// A changing GCLK-pulse count between vsyncs changes the drivers' S-PWM
// truncation -- i.e. visible brightness pulsing (found on the S3). With a
// constant value the frame rate lands a little under the target (each cycle
// also has a few us of PARLIO restart gap); that costs nothing visible.
uint32_t scanout_par_cycle_us(void) { return nominal_cycle_us(); }

// Measured cycle period, for display only (includes restart gaps and vsync
// dark time). Updated at most every 0.5 s by whoever asks for refresh_hz.
static uint32_t measured_cycle_us(void)
{
    uint32_t nominal = nominal_cycle_us();
    int64_t now = esp_timer_get_time();
    uint32_t done = s_done;
    if (!scanout_par_running() || s_meas_t0 == 0) {
        s_meas_t0 = now;
        s_meas_c0 = done;
    } else if (now - s_meas_t0 >= 500000) {
        uint32_t dc = done - s_meas_c0;
        if (dc > 50) {
            uint32_t m = (uint32_t)((now - s_meas_t0) / dc);
            if (m >= nominal && m <= nominal + nominal / 4) s_cycle_us_meas = m;
        }
        s_meas_t0 = now;
        s_meas_c0 = done;
    }
    return s_cycle_us_meas ? s_cycle_us_meas : nominal;
}
