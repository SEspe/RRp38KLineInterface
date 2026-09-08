idf_component_register(
    SRCS "main.c" "kline.c" "kline_console.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_uart esp_driver_gpio console esp_timer esp_rom soc
)
