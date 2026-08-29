idf_component_register(
    SRCS "agent_robot_terminal.c"
    INCLUDE_DIRS "."
    REQUIRES device_connection device_identity esp_event esp_netif network_connectivity nvs_flash
)
