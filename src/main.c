// COS-102 LED sign controller for the ESP32-S31-Function-CoreBoard-1 + hat:
// decoded-protocol driver + browser UI over WiFi.
// See ../../LEDSIGNPART3/docs/PROTOCOL.md for the reverse-engineered protocol.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "datapath.h"
#include "datapath_qspi.h"
#include "scanout.h"
#include "scanout_par.h"
#include "protocol.h"
#include "framebuf.h"
#include "console.h"
#include "settings.h"
#include "canvas.h"
#include "effects.h"
#include "render.h"
#include "wifi_mgr.h"
#include "webserver.h"
#include "replay_data.h"
#include "beatsync.h"

#define TAG "main"

// Panel bring-up runs as a task pinned to core 1 so the peripherals' DMA
// interrupts (PARLIO scan, SPI data) are allocated on core 1 -- the same core
// as the display task, and away from WiFi/lwIP/HTTP on core 0. An ISR delayed
// by the WiFi task delays the vsync, which shows up as stutter and brightness
// pulsing. ESP-IDF binds an interrupt to the core that allocates it.
static void panel_bringup(void *arg)
{
    dp_init();
    scanout_init();
    proto_init();
    map_default();
    fb_clear();
    canvas_clear();

    // Apply persisted bus tuning (overclocking) before the scan unit is built.
    if (g_set.tune_pclk_hz && g_set.tune_burst) {
        g_tune.pclk_hz = g_set.tune_pclk_hz;
        g_tune.burst = g_set.tune_burst;
        g_tune.pad = g_set.tune_pad;
    }
    if (!scanout_set_backend(SCAN_BACKEND_PARLIO)) {
        ESP_LOGW(TAG, "PARLIO scanout unavailable, falling back to bit-bang");
    }
    scanout_start();

    if (g_set.tune_data_hz) dp_qspi_set_clock((int)g_set.tune_data_hz);
    if (dp_qspi_init()) g_data_backend = DATA_BACKEND_QSPI;
    else ESP_LOGW(TAG, "quad-SPI unavailable, data path stays on bit-bang");

    proto_boot();
    xTaskNotifyGive((TaskHandle_t)arg);
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_init();

    xTaskCreatePinnedToCore(panel_bringup, "bringup", 4096,
                            xTaskGetCurrentTaskHandle(), 10, NULL, 1);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // --- render engine + network ---
    render_init();
    render_start();
    wifi_mgr_init();
    webserver_start();
    beatsync_start();

    printf("\n=== COS-102 LED sign controller (ESP32-S31) ===\n");
    printf("panel   : %dx%d, %d drivers/color, %d-bit shifts, 1/%d scan\n",
           PANEL_W, PANEL_H, CHAIN_DRIVERS, BITS_PER_SHIFT, SCAN_PHASES);
    printf("bus     : scan %s, GCLK %.2f MHz, %.0f Hz refresh; data %s %.2f MHz\n",
           scanout_get_backend() == SCAN_BACKEND_PARLIO ? "PARLIO DMA" : "bit-bang",
           g_tune.pclk_hz / 2e6, scanout_par_refresh_hz(),
           g_data_backend == DATA_BACKEND_QSPI ? "quad-SPI" : "bit-bang",
           dp_qspi_get_clock() / 1e6);
    printf("effects : %d  text animations: %d\n", g_effect_count, g_textanim_count);
    printf("web UI  : http://%s/   (type 'help' for the serial console)\n",
           wifi_ip_str());

    console_start();
}
