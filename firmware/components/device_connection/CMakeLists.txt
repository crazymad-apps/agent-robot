idf_component_register(
    SRCS "device_connection.c" "device_connection_supervisor.c"
    INCLUDE_DIRS "include"
    REQUIRES device_protocol esp_timer host_discovery wss_transport
)
