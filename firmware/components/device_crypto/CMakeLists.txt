idf_component_register(
    SRCS
        "device_base64url.c"
        "device_ed25519.c"
        "device_transcript.c"
        "third_party/monocypher/monocypher.c"
        "third_party/monocypher/monocypher-ed25519.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "third_party/monocypher"
    REQUIRES device_protocol
)
