idf_component_register(SRCS "main.c" "status_display.c" "audio_test.c" "display_test.c" "mic_level_test.c" "wifi_manager.c" "wakenet_listener.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_i2s esp_driver_gpio esp_driver_spi esp_driver_usb_serial_jtag esp_event esp_lcd esp_netif esp_partition esp_timer esp_wifi espressif__esp-sr led_strip lvgl nvs_flash spi_flash)
