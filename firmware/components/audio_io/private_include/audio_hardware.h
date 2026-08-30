#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct audio_hardware audio_hardware_t;

typedef enum {
    AUDIO_HARDWARE_RESULT_OK = 0,
    AUDIO_HARDWARE_RESULT_INVALID_ARGUMENT,
    AUDIO_HARDWARE_RESULT_NO_MEMORY,
    AUDIO_HARDWARE_RESULT_INVALID_STATE,
    AUDIO_HARDWARE_RESULT_TIMEOUT,
    AUDIO_HARDWARE_RESULT_DRIVER_FAILURE,
} audio_hardware_result_t;

audio_hardware_result_t audio_hardware_create(audio_hardware_t **hardware);

audio_hardware_result_t audio_hardware_capture_start(audio_hardware_t *hardware);

audio_hardware_result_t audio_hardware_capture_read(audio_hardware_t *hardware, int32_t *i2s_words,
                                                    size_t i2s_word_capacity,
                                                    size_t *i2s_word_count, uint32_t timeout_ms);

audio_hardware_result_t audio_hardware_capture_stop(audio_hardware_t *hardware);

audio_hardware_result_t audio_hardware_playback_start(audio_hardware_t *hardware);

audio_hardware_result_t audio_hardware_playback_write(audio_hardware_t *hardware,
                                                      const int32_t *i2s_words,
                                                      size_t i2s_word_count, uint32_t timeout_ms);

audio_hardware_result_t audio_hardware_playback_stop(audio_hardware_t *hardware);

void audio_hardware_destroy(audio_hardware_t *hardware);
