idf_component_register(
    SRCS "voice_input.c"
    INCLUDE_DIRS "include"
    REQUIRES device_connection esp_timer heap utterance_cache
)
