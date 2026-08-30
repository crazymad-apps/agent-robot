#include "utterance_cache.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_pcm_frame.h"
#include "utterance_cache_memory.h"

static uint8_t *allocated_memory;
static size_t allocated_size;

uint8_t *utterance_cache_memory_allocate(size_t size)
{
    assert(allocated_memory == NULL);
    allocated_memory = calloc(1U, size);
    allocated_size = size;
    return allocated_memory;
}

void utterance_cache_memory_release(uint8_t *memory)
{
    assert(memory == allocated_memory);
    for (size_t index = 0U; index < allocated_size; ++index) {
        assert(memory[index] == 0U);
    }
    free(memory);
    allocated_memory = NULL;
    allocated_size = 0U;
}

static void verify_seal_and_expiry(void)
{
    utterance_cache_t *cache = NULL;
    assert(utterance_cache_create(&cache) == UTTERANCE_CACHE_RESULT_OK);
    assert(utterance_cache_begin(cache, "client-input", DEVICE_OUTPUT_PREFERENCE_AUDIO) ==
           UTTERANCE_CACHE_RESULT_OK);
    const int16_t samples[] = {1, -2, 3};
    assert(utterance_cache_append(cache, samples, sizeof(samples) / sizeof(samples[0])) ==
           UTTERANCE_CACHE_RESULT_OK);
    assert(utterance_cache_seal(cache, 1000U) == UTTERANCE_CACHE_RESULT_OK);

    utterance_cache_view_t view = {0};
    assert(utterance_cache_copy_view(cache, &view) == UTTERANCE_CACHE_RESULT_OK);
    assert(view.pcm_size == DEVICE_PCM_PAYLOAD_BYTES);
    assert(view.source_sample_count == 3U);
    assert(strcmp(view.client_input_id, "client-input") == 0);
    assert(view.output_preference == DEVICE_OUTPUT_PREFERENCE_AUDIO);
    assert(view.sealed_at_ms == 1000U);
    assert(memcmp(view.pcm, samples, sizeof(samples)) == 0);
    for (size_t index = sizeof(samples); index < view.pcm_size; ++index) {
        assert(view.pcm[index] == 0U);
    }
    assert(!utterance_cache_is_expired(cache, 120999U));
    assert(utterance_cache_is_expired(cache, 121000U));

    utterance_cache_clear(cache);
    assert(utterance_cache_state(cache) == UTTERANCE_CACHE_STATE_EMPTY);
    assert(utterance_cache_copy_view(cache, &view) == UTTERANCE_CACHE_RESULT_INVALID_STATE);
    utterance_cache_destroy(cache);
}

static void verify_capacity_boundary(void)
{
    utterance_cache_t *cache = NULL;
    assert(utterance_cache_create(&cache) == UTTERANCE_CACHE_RESULT_OK);
    assert(utterance_cache_begin(cache, "client-input", DEVICE_OUTPUT_PREFERENCE_AUDIO) ==
           UTTERANCE_CACHE_RESULT_OK);
    int16_t frame_samples[DEVICE_PCM_PAYLOAD_BYTES / sizeof(int16_t)] = {0};
    const size_t frame_count = UTTERANCE_CACHE_MAX_PCM_BYTES / DEVICE_PCM_PAYLOAD_BYTES;
    for (size_t index = 0U; index < frame_count; ++index) {
        assert(utterance_cache_append(cache, frame_samples,
                                      sizeof(frame_samples) / sizeof(frame_samples[0])) ==
               UTTERANCE_CACHE_RESULT_OK);
    }
    assert(utterance_cache_append(cache, frame_samples, 1U) ==
           UTTERANCE_CACHE_RESULT_CAPACITY_EXCEEDED);
    assert(utterance_cache_seal(cache, 0U) == UTTERANCE_CACHE_RESULT_OK);
    utterance_cache_view_t view = {0};
    assert(utterance_cache_copy_view(cache, &view) == UTTERANCE_CACHE_RESULT_OK);
    assert(view.pcm_size == UTTERANCE_CACHE_MAX_PCM_BYTES);
    assert(view.source_sample_count == UTTERANCE_CACHE_MAX_SAMPLE_COUNT);
    utterance_cache_destroy(cache);
}

static void verify_state_boundaries(void)
{
    utterance_cache_t *cache = NULL;
    assert(utterance_cache_create(&cache) == UTTERANCE_CACHE_RESULT_OK);
    assert(utterance_cache_begin(cache, " ", DEVICE_OUTPUT_PREFERENCE_AUDIO) ==
           UTTERANCE_CACHE_RESULT_INVALID_ARGUMENT);
    assert(utterance_cache_begin(cache, "client-input", DEVICE_OUTPUT_PREFERENCE_AUDIO) ==
           UTTERANCE_CACHE_RESULT_OK);
    assert(utterance_cache_begin(cache, "second-input", DEVICE_OUTPUT_PREFERENCE_AUDIO) ==
           UTTERANCE_CACHE_RESULT_INVALID_STATE);
    assert(utterance_cache_seal(cache, 0U) == UTTERANCE_CACHE_RESULT_INVALID_STATE);
    utterance_cache_destroy(cache);
}

int main(void)
{
    verify_seal_and_expiry();
    verify_capacity_boundary();
    verify_state_boundaries();
    puts("utterance cache lifecycle and capacity checks passed");
    return 0;
}
