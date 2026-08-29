idf_component_register(
    SRCS "target_device_connection_recovery_validation.c"
    INCLUDE_DIRS "."
    REQUIRES
        device_connection
        esp_event
        esp_netif
        network_connectivity
        nvs_flash
)
