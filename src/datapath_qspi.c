// Quad-SPI DMA data path: R/G/B shift in parallel on SPI2 IO0/IO1/IO2 with
// the shared CLK from SCLK -- 800 clocks per slot (40 us at 20 MHz vs ~400 us
// bit-banged). LAT is pulsed by GPIO after each transfer (the "latch in
// CLK-idle" style verified on hardware and in the boot captures).
//
// The CLK/R/G/B pins are shared with the bit-bang engine (used for vsync and
// config commands, which need LE spanning CLK edges). dp_qspi_route_spi() /
// dp_qspi_route_gpio() switch the GPIO matrix between the two owners.
//
// ESP32-S31 notes:
//  - The hat's pins are not SPI2's IO_MUX pins, so everything goes through
//    the GPIO matrix (fine to 40 MHz; the panel tops out near 20 MHz).
//  - Quad mode needs an IO3 pin at init. It gets a spare J2 pin that is reset
//    right after init, so nothing on the header toggles.
//  - The DMA buffer comes from spi_bus_dma_memory_alloc(), which meets the
//    S31's DMA alignment rules. A plain malloc'd buffer would make the driver
//    bounce-copy every one of the 128 transfers per frame.
#include "datapath_qspi.h"
#include "datapath.h"
#include "pins.h"
#include "replay_data.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "soc/spi_periph.h"
#include "soc/gpio_sig_map.h"
#include "soc/gpio_reg.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "esp_log.h"

#define TAG "qspi"
#define QSPI_HZ 20000000     // measured stable on this panel; OEM shifted at 12.5 MHz

static spi_device_handle_t s_dev;
static spi_transaction_t s_tx;  // reused across slots
static uint8_t *s_dma_buf;      // 400 bytes = 800 nibbles = one slot
static bool s_ready;
static bool s_nib_high_first = true;   // quad nibble order; `data niborder`
static int s_req_hz = QSPI_HZ;         // requested clock
static int s_clock_hz = QSPI_HZ;       // actual clock (what the divider gave)
static void build_exp_tables(void);

int dp_qspi_get_clock(void) { return s_clock_hz; }

static esp_err_t add_device(int hz)
{
    spi_device_interface_config_t dev = {
        .clock_speed_hz = hz,
        .mode = 0,                       // data stable before rising edge
        .spics_io_num = -1,
        .queue_size = 4,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    esp_err_t err = spi_bus_add_device(SPI2_HOST, &dev, &s_dev);
    if (err != ESP_OK) return err;
    int khz = 0;
    if (spi_device_get_actual_freq(s_dev, &khz) == ESP_OK && khz > 0)
        s_clock_hz = khz * 1000;
    else
        s_clock_hz = hz;
    s_req_hz = hz;
    return ESP_OK;
}

// Re-add the SPI device at a new clock (overclocking; datasheet max 30 MHz).
bool dp_qspi_set_clock(int hz)
{
    if (hz < 1000000 || hz > 40000000) return false;
    if (!s_ready) { s_req_hz = s_clock_hz = hz; return true; }
    spi_bus_remove_device(s_dev);
    if (add_device(hz) != ESP_OK) {
        add_device(s_req_hz);            // put the old one back
        return false;
    }
    dp_qspi_route_gpio();
    if (s_clock_hz != hz)
        ESP_LOGI(TAG, "data clock %d Hz requested, %d Hz actual", hz, s_clock_hz);
    return true;
}

bool dp_qspi_init(void)
{
    if (s_ready) return true;
    spi_bus_config_t bus = {
        .data0_io_num = PIN_DATA_R,
        .data1_io_num = PIN_DATA_G,
        .data2_io_num = PIN_DATA_B,
        .data3_io_num = PIN_QSPI_D3_SPARE,
        .sclk_io_num = PIN_CLK,
        .max_transfer_sz = 512,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = add_device(s_req_hz);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
        spi_bus_free(SPI2_HOST);
        return false;
    }
    s_dma_buf = spi_bus_dma_memory_alloc(SPI2_HOST, BITS_PER_SHIFT / 2, 0);
    if (!s_dma_buf) {
        ESP_LOGE(TAG, "DMA buffer alloc failed");
        spi_bus_remove_device(s_dev);
        spi_bus_free(SPI2_HOST);
        return false;
    }
    build_exp_tables();
    s_ready = true;
    // IO3 is never used: give the spare pin straight back (input, undriven).
    gpio_reset_pin(PIN_QSPI_D3_SPARE);
    // spi_bus_initialize routed the pins to the peripheral; hand them back to
    // GPIO until a frame actually goes out (commands/latches need them).
    dp_qspi_route_gpio();
    ESP_LOGI(TAG, "quad-SPI data path ready at %d Hz", s_clock_hz);
    return true;
}

void dp_qspi_route_spi(void)
{
    const spi_signal_conn_t *p = &spi_periph_signal[SPI2_HOST];
    esp_rom_gpio_connect_out_signal(PIN_CLK,    p->spiclk_out, false, false);
    esp_rom_gpio_connect_out_signal(PIN_DATA_R, p->spid_out,   false, false);
    esp_rom_gpio_connect_out_signal(PIN_DATA_G, p->spiq_out,   false, false);
    esp_rom_gpio_connect_out_signal(PIN_DATA_B, p->spiwp_out,  false, false);
}

void dp_qspi_route_gpio(void)
{
    const int pins[] = { PIN_CLK, PIN_DATA_R, PIN_DATA_G, PIN_DATA_B };
    for (int i = 0; i < 4; i++) {
        esp_rom_gpio_connect_out_signal(pins[i], SIG_GPIO_OUT_IDX, false, false);
        gpio_set_direction(pins[i], GPIO_MODE_OUTPUT);
    }
    REG_WRITE(GPIO_OUT1_W1TC_REG,
              BANK1_BIT(PIN_CLK) | BANK1_BIT(PIN_DATA_R) |
              BANK1_BIT(PIN_DATA_G) | BANK1_BIT(PIN_DATA_B));
}

void dp_qspi_set_niborder(bool high_first) { s_nib_high_first = high_first; }
bool dp_qspi_get_niborder(void) { return s_nib_high_first; }

// Byte -> 8 nibbles, one bit per nibble, MSB of the byte in the first nibble.
// Lets the packer handle 8 bits of all three colours with 3 table lookups
// instead of 8 iterations of shift/mask/branch -- the per-bit loop cost ~70 us
// per slot (9 ms per frame) on the S3, which was most of the frame budget.
static uint32_t s_exp_hi[256];   // first nibble in the high half of byte 0
static uint32_t s_exp_lo[256];   // first nibble in the low half

static void build_exp_tables(void)
{
    for (int v = 0; v < 256; v++) {
        uint32_t hi = 0, lo = 0;
        for (int i = 0; i < 8; i++) {
            if (!((v >> (7 - i)) & 1)) continue;
            hi |= 1u << ((i >> 1) * 8 + ((i & 1) ? 0 : 4));
            lo |= 1u << ((i >> 1) * 8 + ((i & 1) ? 4 : 0));
        }
        s_exp_hi[v] = hi;
        s_exp_lo[v] = lo;
    }
}

// Pack one slot into quad nibbles: bit i of the 800-bit shift -> nibble
// {IO0=R, IO1=G, IO2=B}, MSB of each 16-bit word first.
static void IRAM_ATTR pack_slot(const uint16_t *r, const uint16_t *g,
                                const uint16_t *b)
{
    const uint32_t *exp = s_nib_high_first ? s_exp_hi : s_exp_lo;
    uint32_t *out = (uint32_t *)s_dma_buf;
    for (int w = 0; w < WORDS_PER_SHIFT; w++) {
        uint32_t rv = r[w], gv = g[w], bv = b[w];
        *out++ = exp[rv >> 8]   | (exp[gv >> 8] << 1)   | (exp[bv >> 8] << 2);
        *out++ = exp[rv & 0xff] | (exp[gv & 0xff] << 1) | (exp[bv & 0xff] << 2);
    }
}

// Batch bracket: holding the bus across the whole frame removes a device
// acquire/release from every one of the 128 per-frame transactions.
void dp_qspi_begin(void)
{
    if (!s_ready) return;
    spi_device_acquire_bus(s_dev, portMAX_DELAY);
    dp_qspi_route_spi();
}

void dp_qspi_end(void)
{
    if (!s_ready) return;
    dp_qspi_route_gpio();      // commands/vsync need GPIO bit-bang
    spi_device_release_bus(s_dev);
}

bool dp_qspi_send_slot(const uint16_t *r, const uint16_t *g, const uint16_t *b)
{
    if (!s_ready) return false;
    pack_slot(r, g, b);
    s_tx.flags = SPI_TRANS_MODE_QIO;
    s_tx.length = BITS_PER_SHIFT * 4;   // bits across 4 lines: 800 clocks
    s_tx.tx_buffer = s_dma_buf;
    if (spi_device_polling_start(s_dev, &s_tx, portMAX_DELAY) != ESP_OK) return false;
    if (spi_device_polling_end(s_dev, portMAX_DELAY) != ESP_OK) return false;
    // LAT pulse in CLK-idle (LAT stays GPIO-owned). Keep it comfortably wide:
    // a marginal pulse risks being sampled inconsistently across the 50-driver
    // chain, and a mis-timed LE next to clock edges is how a data latch turns
    // into a *config command* -- which sticks until the drivers are re-inited.
    // 128 slots x 1 us = 0.13 ms per frame, irrelevant next to the send.
    REG_WRITE(GPIO_OUT1_W1TS_REG, BANK1_BIT(PIN_LAT));
    esp_rom_delay_us(1);
    REG_WRITE(GPIO_OUT1_W1TC_REG, BANK1_BIT(PIN_LAT));
    esp_rom_delay_us(1);
    return true;
}
