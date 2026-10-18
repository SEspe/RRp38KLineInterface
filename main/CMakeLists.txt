idf_component_register(
    SRCS "main.c" "kline.c" "kline_console.c" "net.c" "sniff.c" "slowlog.c" "waveform.c" "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_uart esp_driver_gpio console esp_timer esp_rom soc esp_hw_support
             esp_wifi esp_event esp_netif nvs_flash esp_http_server app_update
             esp_http_client esp_https_ota esp-tls mbedtls
)
