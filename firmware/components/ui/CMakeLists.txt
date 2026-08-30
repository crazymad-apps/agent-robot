idf_component_register(
    SRCS "terminal_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES board device_pairing esp_driver_spi esp_lcd esp_timer lvgl
)
