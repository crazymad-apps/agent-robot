idf_component_register(
    SRCS "target_transport_fault_validation.c"
    INCLUDE_DIRS "."
    REQUIRES
        device_connection
        esp_event
        esp_netif
        esp_wifi
        host_discovery
        nvs_flash
        wss_transport
)
