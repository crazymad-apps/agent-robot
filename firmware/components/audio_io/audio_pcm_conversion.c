#include "audio_pcm_conversion.h"

audio_pcm_conversion_result_t audio_pcm_capture_convert(const int32_t *i2s_words,
                                                        size_t i2s_word_count, int16_t *pcm_samples,
                                                        size_t pcm_capacity,
                                                        size_t *pcm_sample_count)
{
    if (i2s_words == NULL || i2s_word_count == 0U ||
        i2s_word_count % AUDIO_PCM_I2S_WORDS_PER_FRAME != 0U || pcm_samples == NULL ||
        pcm_sample_count == NULL) {
        return AUDIO_PCM_CONVERSION_RESULT_INVALID_ARGUMENT;
    }
    const size_t required_samples = i2s_word_count / AUDIO_PCM_I2S_WORDS_PER_FRAME;
    *pcm_sample_count = required_samples;
    if (required_samples > pcm_capacity) {
        return AUDIO_PCM_CONVERSION_RESULT_OUTPUT_TOO_SMALL;
    }
    for (size_t frame_index = 0U; frame_index < required_samples; ++frame_index) {
        const int32_t left_slot = i2s_words[frame_index * AUDIO_PCM_I2S_WORDS_PER_FRAME];
        const uint16_t upper_word = (uint16_t)((uint32_t)left_slot >> 16U);
        const int32_t signed_sample = upper_word >= UINT16_C(0x8000)
                                          ? (int32_t)upper_word - INT32_C(0x10000)
                                          : (int32_t)upper_word;
        pcm_samples[frame_index] = (int16_t)signed_sample;
    }
    return AUDIO_PCM_CONVERSION_RESULT_OK;
}

audio_pcm_conversion_result_t audio_pcm_playback_convert(const int16_t *pcm_samples,
                                                         size_t pcm_sample_count,
                                                         uint8_t gain_percent, int32_t *i2s_words,
                                                         size_t i2s_word_capacity,
                                                         size_t *i2s_word_count)
{
    if (pcm_samples == NULL || pcm_sample_count == 0U || gain_percent > 100U || i2s_words == NULL ||
        i2s_word_count == NULL || pcm_sample_count > SIZE_MAX / AUDIO_PCM_I2S_WORDS_PER_FRAME) {
        return AUDIO_PCM_CONVERSION_RESULT_INVALID_ARGUMENT;
    }
    const size_t required_words = pcm_sample_count * AUDIO_PCM_I2S_WORDS_PER_FRAME;
    *i2s_word_count = required_words;
    if (required_words > i2s_word_capacity) {
        return AUDIO_PCM_CONVERSION_RESULT_OUTPUT_TOO_SMALL;
    }
    for (size_t sample_index = 0U; sample_index < pcm_sample_count; ++sample_index) {
        const int32_t scaled_sample = ((int32_t)pcm_samples[sample_index] * gain_percent) / 100;
        const int32_t slot_value = scaled_sample * 65536;
        i2s_words[sample_index * AUDIO_PCM_I2S_WORDS_PER_FRAME] = slot_value;
        i2s_words[sample_index * AUDIO_PCM_I2S_WORDS_PER_FRAME + 1U] = slot_value;
    }
    return AUDIO_PCM_CONVERSION_RESULT_OK;
}
