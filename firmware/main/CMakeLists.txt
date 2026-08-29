idf_component_register(
    SRCS "agent_robot_terminal.c"
    INCLUDE_DIRS "."
    REQUIRES device_connection host_discovery nvs_flash wss_transport
)
