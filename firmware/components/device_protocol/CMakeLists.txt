idf_component_register(
    SRCS
        "device_capabilities.c"
        "device_control_envelope.c"
        "device_message_replay_window.c"
        "device_pcm_frame.c"
    INCLUDE_DIRS "include"
    REQUIRES espressif__cjson
)
