// Data-side bit-bang engine: CLK + R/G/B + LAT driven by direct register
// writes. On the S31 hat every panel pin is in GPIO bank 1 (32..63), so this
// uses the GPIO_OUT1_W1TS/W1TC registers (the S3 used bank 0).
//
// Used for every command whose LE must span CLK edges (vsync, pre-active,
// config writes) and as the fallback full-frame data path. The fast frame
// path is quad-SPI DMA (datapath_qspi.c).
#include "datapath.h"
#include <stdio.h>
#include "pins.h"
#include "replay_data.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "soc/gpio_reg.h"
#include "esp_attr.h"

#define M_CLK   BANK1_BIT(PIN_CLK)
#define M_R     BANK1_BIT(PIN_DATA_R)
#define M_G     BANK1_BIT(PIN_DATA_G)
#define M_B     BANK1_BIT(PIN_DATA_B)
#define M_LAT   BANK1_BIT(PIN_LAT)
#define M_DATA  (M_R | M_G | M_B)

#define OUT_SET GPIO_OUT1_W1TS_REG
#define OUT_CLR GPIO_OUT1_W1TC_REG

static uint32_t s_nops = 20;

static inline void IRAM_ATTR pause_half(void)
{
    for (uint32_t i = 0; i < s_nops; i++) {
        __asm__ volatile("nop");
    }
}

void dp_set_halfperiod(uint32_t nops) { s_nops = nops; }
uint32_t dp_get_halfperiod(void) { return s_nops; }

void dp_init(void)
{
    const uint64_t mask =
        (1ULL << PIN_CLK) | (1ULL << PIN_DATA_R) | (1ULL << PIN_DATA_G) |
        (1ULL << PIN_DATA_B) | (1ULL << PIN_LAT);
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    REG_WRITE(OUT_CLR, M_CLK | M_DATA | M_LAT);
}

// Shift one bit triple. Data changes while CLK low; driver samples on the
// rising edge (matches measured OEM behavior: data launches after the edge).
// At 320 MHz the S31 core is far faster than the GPIO write path, so the
// effective bit rate is set by the bus writes plus pause_half(), not the CPU.
static inline void IRAM_ATTR shift_bit(uint32_t set_mask)
{
    REG_WRITE(OUT_CLR, M_CLK | M_DATA);
    REG_WRITE(OUT_SET, set_mask);
    pause_half();
    REG_WRITE(OUT_SET, M_CLK);
    pause_half();
}

static void IRAM_ATTR shift_range(const uint16_t *r, const uint16_t *g,
                                  const uint16_t *b, int start_bit, int end_bit)
{
    for (int bit = start_bit; bit < end_bit; bit++) {
        const int w = bit >> 4;
        const uint16_t m = 0x8000u >> (bit & 15);   // MSB first
        uint32_t set = 0;
        if (r[w] & m) set |= M_R;
        if (g[w] & m) set |= M_G;
        if (b[w] & m) set |= M_B;
        shift_bit(set);
    }
}

void dp_shift_words(const uint16_t *r, const uint16_t *g, const uint16_t *b,
                    int nwords)
{
    shift_range(r, g, b, 0, nwords * 16);
    REG_WRITE(OUT_CLR, M_CLK | M_DATA);
}

void dp_shift_latch(const uint16_t *r, const uint16_t *g, const uint16_t *b,
                    int nwords, int code)
{
    const int nbits = nwords * 16;
    if (code <= 0) {
        // shift everything, then latch in CLK-idle (verified working in boot capture)
        shift_range(r, g, b, 0, nbits);
        REG_WRITE(OUT_CLR, M_CLK | M_DATA);
        pause_half();
        REG_WRITE(OUT_SET, M_LAT);
        pause_half(); pause_half();
        REG_WRITE(OUT_CLR, M_LAT);
        pause_half();
        return;
    }
    // OEM style: LE covers the last `code` edges; last payload bit on the
    // final rising edge before LE falls.
    shift_range(r, g, b, 0, nbits - code);
    REG_WRITE(OUT_SET, M_LAT);
    shift_range(r, g, b, nbits - code, nbits);
    REG_WRITE(OUT_CLR, M_LAT);
    REG_WRITE(OUT_CLR, M_CLK | M_DATA);
    pause_half();
}

void dp_latch_only(int code)
{
    REG_WRITE(OUT_CLR, M_CLK | M_DATA);
    pause_half();
    REG_WRITE(OUT_SET, M_LAT);
    for (int i = 0; i < code; i++) {
        REG_WRITE(OUT_CLR, M_CLK);
        pause_half();
        REG_WRITE(OUT_SET, M_CLK);
        pause_half();
    }
    REG_WRITE(OUT_CLR, M_LAT);
    REG_WRITE(OUT_CLR, M_CLK);
    pause_half();
}

void dp_config_write(uint16_t r_val, uint16_t g_val, uint16_t b_val, int code)
{
    uint16_t r[WORDS_PER_SHIFT], g[WORDS_PER_SHIFT], b[WORDS_PER_SHIFT];
    for (int i = 0; i < WORDS_PER_SHIFT; i++) {
        r[i] = r_val; g[i] = g_val; b[i] = b_val;
    }
    dp_shift_latch(r, g, b, WORDS_PER_SHIFT, code);
}

void dp_pin_test(int seconds)
{
    const int pins[] = { PIN_CLK, PIN_DATA_R, PIN_DATA_G, PIN_DATA_B, PIN_LAT };
    const char *names[] = { "CLK", "R", "G", "B", "LAT" };
    for (int s = 0; s < seconds * 2; s++) {
        int i = s % 5;
        printf("toggling %s (GPIO%d)\n", names[i], pins[i]);
        gpio_set_level(pins[i], 1);
        vTaskDelay(pdMS_TO_TICKS(250));
        gpio_set_level(pins[i], 0);
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
