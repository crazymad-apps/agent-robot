idf_component_register(
    SRCS "target_transport_validation.c"
    INCLUDE_DIRS "."
    REQUIRES
        device_connection
        device_protocol
        esp_event
        esp_netif
        host_discovery
        network_connectivity
        nvs_flash
        wss_transport
)
