#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_PCM_I2S_WORDS_PER_FRAME 2U

typedef enum {
    AUDIO_PCM_CONVERSION_RESULT_OK = 0,
    AUDIO_PCM_CONVERSION_RESULT_INVALID_ARGUMENT,
    AUDIO_PCM_CONVERSION_RESULT_OUTPUT_TOO_SMALL,
} audio_pcm_conversion_result_t;

audio_pcm_conversion_result_t audio_pcm_capture_convert(const int32_t *i2s_words,
                                                        size_t i2s_word_count, int16_t *pcm_samples,
                                                        size_t pcm_capacity,
                                                        size_t *pcm_sample_count);

audio_pcm_conversion_result_t audio_pcm_playback_convert(const int16_t *pcm_samples,
                                                         size_t pcm_sample_count,
                                                         uint8_t gain_percent, int32_t *i2s_words,
                                                         size_t i2s_word_capacity,
                                                         size_t *i2s_word_count);

#ifdef __cplusplus
}
#endif
