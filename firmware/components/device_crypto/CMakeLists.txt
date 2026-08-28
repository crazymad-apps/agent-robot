idf_component_register(
    SRCS "device_base64url.c" "device_transcript.c"
    INCLUDE_DIRS "include"
    REQUIRES device_protocol
)
