idf_component_register(
    SRCS "audio_hardware.c" "audio_pcm_conversion.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES board esp_driver_gpio esp_driver_i2s
)
