idf_component_register(
    SRCS "agent_robot_terminal.c"
    INCLUDE_DIRS "."
    REQUIRES esp_event esp_netif network_connectivity nvs_flash
)
