idf_component_register(
    SRCS "network_connectivity.c" "network_reconnect_policy.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES esp_event esp_netif esp_timer esp_wifi nvs_flash
)
