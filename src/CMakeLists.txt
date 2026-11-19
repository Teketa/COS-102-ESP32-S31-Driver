idf_component_register(
    SRCS
        main.c
        datapath.c
        datapath_qspi.c
        scanout.c
        scanout_par.c
        protocol.c
        framebuf.c
        patterns.c
        console.c
        settings.c
        font.c
        canvas.c
        effects.c
        effects_art.c
        effects_theme.c
        render.c
        wifi_mgr.c
        webserver.c
        web_index.c
        beatclock.c
        beatsync.c
        samples.c
    INCLUDE_DIRS "."
    REQUIRES esp_driver_gpio esp_driver_spi esp_hal_gpspi esp_driver_parlio
             esp_hw_support console esp_timer nvs_flash esp_wifi esp_netif
             esp_event esp_http_server lwip espressif__cjson espressif__mdns
)
