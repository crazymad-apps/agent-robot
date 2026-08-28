idf_component_register(
    SRCS "main.c" "voice_demo_client.c" "wifi_station.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_gpio
        esp_driver_i2s
        esp_event
        esp_netif
        esp_timer
        esp_wifi
        espressif__cjson
        espressif__esp_websocket_client
        espressif__led_strip
        nvs_flash
)
