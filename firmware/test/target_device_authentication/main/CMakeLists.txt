idf_component_register(
    SRCS "target_device_authentication_validation.c"
    INCLUDE_DIRS "."
    REQUIRES
        device_connection
        device_crypto
        device_identity
        esp_event
        esp_netif
        host_discovery
        network_connectivity
        nvs_flash
)

target_compile_definitions(
    __idf_device_identity
    PRIVATE DEVICE_IDENTITY_NVS_NAMESPACE="dev_auth_val"
)
