idf_component_register(
    SRCS
        "app_main.c"
        "ws_client.c"
        "protocol.c"
        "audio.c"
        "ui.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_netif nvs_flash esp_event json esp_websocket_client
)
