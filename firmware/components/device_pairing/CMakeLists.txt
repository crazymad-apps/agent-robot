idf_component_register(
    SRCS "device_pairing.c" "device_pairing_protocol.c"
    INCLUDE_DIRS "include"
    REQUIRES device_crypto device_identity device_protocol espressif__cjson
)
