idf_component_register(
    SRCS "target_network_connectivity_validation.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_event
        esp_netif
        esp_wifi
        network_connectivity
        nvs_flash
)
