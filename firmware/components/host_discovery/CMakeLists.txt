idf_component_register(
    SRCS
        "host_discovery.c"
        "host_selection.c"
    INCLUDE_DIRS "include"
    REQUIRES device_protocol espressif__mdns esp_netif
)
