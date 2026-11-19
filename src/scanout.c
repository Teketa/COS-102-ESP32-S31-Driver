#include "scanout.h"
#include "scanout_par.h"
#include "pins.h"
#include "replay_data.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_rom_gpio.h"

static volatile int s_backend = SCAN_BACKEND_TIMER;
static bool s_par_detached;   // pins handed from PARLIO to bit-bang

// All scan pins are in GPIO bank 1 on the S31 hat (see pins.h).
#define M_GCLK  BANK1_BIT(PIN_GCLK)
#define M_AA    BANK1_BIT(PIN_ADDR_A)
#define M_AB    BANK1_BIT(PIN_ADDR_B)
#define M_AC    BANK1_BIT(PIN_ADDR_C)
#define OUT_SET GPIO_OUT1_W1TS_REG
#define OUT_CLR GPIO_OUT1_W1TC_REG
#define M_ADDR  (M_AA | M_AB | M_AC)

static volatile bool s_run = false;
static volatile bool s_pause_req = false;
static volatile bool s_paused = false;
static volatile uint32_t s_burst = OEM_GCLK_BURST;
static volatile uint32_t s_nops = 10;
static volatile int s_hold = -1;
static TaskHandle_t s_task = NULL;

static inline void IRAM_ATTR gpause(void)
{
    for (uint32_t i = 0, n = s_nops; i < n; i++) __asm__ volatile("nop");
}

void scanout_set_addr(int phase)
{
    uint32_t set = 0;
    if (phase & 1) set |= M_AA;
    if (phase & 2) set |= M_AB;
    if (phase & 4) set |= M_AC;
    REG_WRITE(OUT_CLR, M_ADDR & ~set);
    REG_WRITE(OUT_SET, set);
}

static void IRAM_ATTR gclk_burst(uint32_t pulses)
{
    for (uint32_t i = 0; i < pulses; i++) {
        REG_WRITE(OUT_SET, M_GCLK);
        gpause();
        REG_WRITE(OUT_CLR, M_GCLK);
        gpause();
    }
}

static void scan_task(void *arg)
{
    int phase = 0;
    for (;;) {
        // Honor pause requests only at the scan-cycle boundary (after phase 7
        // completes). The drivers derive frame alignment from vsync relative
        // to the scan; pausing mid-cycle gives each frame a random row offset
        // (observed on hardware as the image walking by one row per frame).
        if (!s_run || (s_pause_req && phase == 0)) {
            REG_WRITE(OUT_CLR, M_GCLK);
            s_paused = true;
            vTaskDelay(1);
            continue;
        }
        s_paused = false;
        int p = (s_hold >= 0) ? s_hold : phase;
        scanout_set_addr(p);
        esp_rom_delay_us(2);           // addr settle in the GCLK gap (OEM: ~3us gap)
        gclk_burst(s_burst);
        esp_rom_delay_us(1);
        phase = (phase + 1) & 7;
        // Yield briefly once per full scan cycle so core 1 housekeeping runs.
        if (phase == 0) taskYIELD();
    }
}

void scanout_init(void)
{
    const uint64_t mask = (1ULL << PIN_GCLK) | (1ULL << PIN_ADDR_A) |
                          (1ULL << PIN_ADDR_B) | (1ULL << PIN_ADDR_C);
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    REG_WRITE(OUT_CLR, M_GCLK | M_ADDR);
    xTaskCreatePinnedToCore(scan_task, "scanout", 4096, NULL,
                            configMAX_PRIORITIES - 2, &s_task, 1);
}

void scanout_start(void)
{
    if (s_backend == SCAN_BACKEND_PARLIO) scanout_par_start();
    else s_run = true;
}

void scanout_stop(void)
{
    if (s_backend == SCAN_BACKEND_PARLIO) scanout_par_stop();
    else s_run = false;
}

bool scanout_running(void)
{
    if (s_backend == SCAN_BACKEND_PARLIO) return scanout_par_running();
    return s_run && !s_paused;
}

void scanout_pause(void)
{
    if (s_backend == SCAN_BACKEND_PARLIO) {
        if (scanout_par_running()) scanout_par_pause_wait();
        return;
    }
    if (!s_run) return;
    s_pause_req = true;
    while (!s_paused) vTaskDelay(1);
}

void scanout_resume(void)
{
    if (s_backend == SCAN_BACKEND_PARLIO) { scanout_par_resume(); return; }
    s_pause_req = false;
}

bool scanout_set_backend(int backend)
{
    if (backend == s_backend) return true;
    // stop whichever is active first
    scanout_stop();
    vTaskDelay(pdMS_TO_TICKS(5));
    if (backend == SCAN_BACKEND_PARLIO) {
        if (!scanout_par_init()) return false;
        // A previous switch to bit-bang took the pins off PARLIO; rebuilding
        // the unit routes them back (init alone only does that on creation).
        if (s_par_detached) {
            scan_tuning_t t = g_tune;
            if (!scanout_par_set_tuning(&t)) return false;
            s_par_detached = false;
        }
        s_backend = SCAN_BACKEND_PARLIO;
    } else {
        // reclaim GCLK/ADDR pins from the PARLIO peripheral for GPIO bit-bang
        const int pins[] = { PIN_GCLK, PIN_ADDR_A, PIN_ADDR_B, PIN_ADDR_C };
        for (int i = 0; i < 4; i++) {
            esp_rom_gpio_connect_out_signal(pins[i], SIG_GPIO_OUT_IDX, false, false);
            gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
        }
        REG_WRITE(OUT_CLR, M_GCLK | M_ADDR);
        s_par_detached = true;
        s_backend = SCAN_BACKEND_TIMER;
    }
    return true;
}

int scanout_get_backend(void) { return s_backend; }

void scanout_set_burst(uint32_t pulses) { s_burst = pulses; }
uint32_t scanout_get_burst(void)        { return s_burst; }
void scanout_set_halfperiod(uint32_t n) { s_nops = n; }
void scanout_set_hold(int phase)        { s_hold = (phase >= 0 && phase <= 7) ? phase : -1; }
int  scanout_get_hold(void)             { return s_hold; }
