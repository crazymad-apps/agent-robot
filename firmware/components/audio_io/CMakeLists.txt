idf_component_register(
    SRCS
        "audio_io.c"
        "audio_hardware.c"
        "audio_pcm_conversion.c"
        "audio_playback_queue.c"
        "audio_playback_queue_memory.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES board device_protocol esp_driver_gpio esp_driver_i2s esp_timer freertos heap
)
