idf_component_register(
    SRCS "device_identity.c" "device_identity_record.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES device_crypto host_discovery nvs_flash
)
