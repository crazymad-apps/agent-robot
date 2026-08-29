idf_component_register(
    SRCS
        "wss_frame_codec.c"
        "wss_transport.c"
        "wss_upgrade.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES device_protocol esp_hw_support esp_timer lwip mbedtls
)
