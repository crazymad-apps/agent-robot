idf_component_register(
    SRCS "agent_robot_terminal.c"
    INCLUDE_DIRS "."
    REQUIRES audio_io device_connection device_identity esp_app_format esp_event esp_netif esp_system interaction network_connectivity nvs_flash ui voice_input
)
