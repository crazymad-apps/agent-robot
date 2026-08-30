#include "audio_pcm_conversion.h"

#include <assert.h>
#include <stdio.h>

#define TEST_PLAYBACK_GAIN_PERCENT 75U

static void verify_capture_conversion(void)
{
    const int32_t i2s_words[] = {INT32_C(0x7FFF0000),           INT32_C(0x11110000),
                                 (int32_t)UINT32_C(0x80000000), INT32_C(0x22220000),
                                 INT32_C(0x00010000),           INT32_C(0x33330000)};
    int16_t pcm_samples[3] = {0};
    size_t pcm_sample_count = 0U;
    assert(audio_pcm_capture_convert(i2s_words, sizeof(i2s_words) / sizeof(i2s_words[0]),
                                     pcm_samples, sizeof(pcm_samples) / sizeof(pcm_samples[0]),
                                     &pcm_sample_count) == AUDIO_PCM_CONVERSION_RESULT_OK);
    assert(pcm_sample_count == 3U);
    assert(pcm_samples[0] == INT16_MAX);
    assert(pcm_samples[1] == INT16_MIN);
    assert(pcm_samples[2] == 1);

    assert(audio_pcm_capture_convert(
               i2s_words, 3U, pcm_samples, sizeof(pcm_samples) / sizeof(pcm_samples[0]),
               &pcm_sample_count) == AUDIO_PCM_CONVERSION_RESULT_INVALID_ARGUMENT);
    assert(audio_pcm_capture_convert(i2s_words, sizeof(i2s_words) / sizeof(i2s_words[0]),
                                     pcm_samples, 2U, &pcm_sample_count) ==
           AUDIO_PCM_CONVERSION_RESULT_OUTPUT_TOO_SMALL);
    assert(pcm_sample_count == 3U);
}

static void verify_playback_conversion(void)
{
    const int16_t pcm_samples[] = {INT16_MIN, -1, 0, 1, INT16_MAX};
    int32_t i2s_words[10] = {0};
    size_t i2s_word_count = 0U;
    assert(audio_pcm_playback_convert(pcm_samples, sizeof(pcm_samples) / sizeof(pcm_samples[0]),
                                      TEST_PLAYBACK_GAIN_PERCENT, i2s_words,
                                      sizeof(i2s_words) / sizeof(i2s_words[0]),
                                      &i2s_word_count) == AUDIO_PCM_CONVERSION_RESULT_OK);
    assert(i2s_word_count == 10U);
    assert(i2s_words[0] == INT32_C(-1610612736));
    assert(i2s_words[1] == i2s_words[0]);
    assert(i2s_words[2] == 0);
    assert(i2s_words[3] == 0);
    assert(i2s_words[8] == INT32_C(1610547200));
    assert(i2s_words[9] == i2s_words[8]);

    assert(audio_pcm_playback_convert(pcm_samples, sizeof(pcm_samples) / sizeof(pcm_samples[0]),
                                      101U, i2s_words, sizeof(i2s_words) / sizeof(i2s_words[0]),
                                      &i2s_word_count) ==
           AUDIO_PCM_CONVERSION_RESULT_INVALID_ARGUMENT);
    assert(audio_pcm_playback_convert(pcm_samples, sizeof(pcm_samples) / sizeof(pcm_samples[0]),
                                      TEST_PLAYBACK_GAIN_PERCENT, i2s_words, 9U,
                                      &i2s_word_count) ==
           AUDIO_PCM_CONVERSION_RESULT_OUTPUT_TOO_SMALL);
    assert(i2s_word_count == 10U);
}

int main(void)
{
    verify_capture_conversion();
    verify_playback_conversion();
    puts("audio PCM capture and playback conversion checks passed");
    return 0;
}
