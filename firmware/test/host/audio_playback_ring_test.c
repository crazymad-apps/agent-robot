#include "audio_playback_ring.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_playback_ring_memory.h"

#define TEST_CONNECTION_EPOCH 7U
#define TEST_STREAM_ID 9U

static uint8_t *allocated_memory;
static size_t allocated_size;

uint8_t *audio_playback_ring_memory_allocate(size_t size)
{
    assert(allocated_memory == NULL);
    allocated_memory = calloc(1U, size);
    allocated_size = size;
    return allocated_memory;
}

void audio_playback_ring_memory_release(uint8_t *memory)
{
    assert(memory == allocated_memory);
    for (size_t index = 0U; index < allocated_size; ++index) {
        assert(memory[index] == 0U);
    }
    free(memory);
    allocated_memory = NULL;
    allocated_size = 0U;
}

static void encode_sample(uint8_t *destination, int16_t sample)
{
    const uint16_t encoded = (uint16_t)sample;
    destination[0] = (uint8_t)encoded;
    destination[1] = (uint8_t)(encoded >> 8U);
}

static device_pcm_frame_view_t frame(uint8_t payload[DEVICE_PCM_PAYLOAD_BYTES], uint32_t sequence,
                                     int16_t first_sample)
{
    memset(payload, 0, DEVICE_PCM_PAYLOAD_BYTES);
    encode_sample(payload, first_sample);
    return (device_pcm_frame_view_t){
        .direction = DEVICE_PCM_DIRECTION_DOWNLINK,
        .stream_id = TEST_STREAM_ID,
        .sequence = sequence,
        .payload = payload,
        .payload_size = DEVICE_PCM_PAYLOAD_BYTES,
    };
}

static void verify_sequence_and_tail(void)
{
    audio_playback_ring_t *ring = NULL;
    assert(audio_playback_ring_create(&ring) == AUDIO_PLAYBACK_RING_RESULT_OK);
    assert(audio_playback_ring_begin(ring, TEST_CONNECTION_EPOCH, TEST_STREAM_ID, 321U) ==
           AUDIO_PLAYBACK_RING_RESULT_OK);
    uint8_t payload[DEVICE_PCM_PAYLOAD_BYTES];
    device_pcm_frame_view_t first = frame(payload, 0U, INT16_MIN);
    assert(audio_playback_ring_push(ring, TEST_CONNECTION_EPOCH, &first) ==
           AUDIO_PLAYBACK_RING_RESULT_OK);
    device_pcm_frame_view_t wrong_sequence = frame(payload, 2U, 5);
    assert(audio_playback_ring_push(ring, TEST_CONNECTION_EPOCH, &wrong_sequence) ==
           AUDIO_PLAYBACK_RING_RESULT_SEQUENCE_MISMATCH);
    device_pcm_frame_view_t second = frame(payload, 1U, INT16_MAX);
    assert(audio_playback_ring_push(ring, TEST_CONNECTION_EPOCH, &second) ==
           AUDIO_PLAYBACK_RING_RESULT_OK);
    assert(audio_playback_ring_end(ring, TEST_CONNECTION_EPOCH, TEST_STREAM_ID) ==
           AUDIO_PLAYBACK_RING_RESULT_OK);
    assert(audio_playback_ring_ready(ring));

    int16_t samples[AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME];
    size_t sample_count = 0U;
    assert(audio_playback_ring_pop(ring, samples, &sample_count) == AUDIO_PLAYBACK_RING_RESULT_OK);
    assert(sample_count == AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME);
    assert(samples[0] == INT16_MIN);
    assert(audio_playback_ring_pop(ring, samples, &sample_count) == AUDIO_PLAYBACK_RING_RESULT_OK);
    assert(sample_count == 1U);
    assert(samples[0] == INT16_MAX);
    assert(samples[1] == 0);
    assert(audio_playback_ring_drained(ring));
    audio_playback_ring_destroy(ring);
}

static void verify_overflow_and_isolation(void)
{
    audio_playback_ring_t *ring = NULL;
    assert(audio_playback_ring_create(&ring) == AUDIO_PLAYBACK_RING_RESULT_OK);
    const uint64_t sample_count =
        (uint64_t)(AUDIO_PLAYBACK_RING_FRAME_CAPACITY + 1U) * AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME;
    assert(audio_playback_ring_begin(ring, TEST_CONNECTION_EPOCH, TEST_STREAM_ID, sample_count) ==
           AUDIO_PLAYBACK_RING_RESULT_OK);
    uint8_t payload[DEVICE_PCM_PAYLOAD_BYTES];
    for (uint32_t sequence = 0U; sequence < AUDIO_PLAYBACK_RING_FRAME_CAPACITY; ++sequence) {
        device_pcm_frame_view_t current = frame(payload, sequence, (int16_t)sequence);
        assert(audio_playback_ring_push(ring, TEST_CONNECTION_EPOCH, &current) ==
               AUDIO_PLAYBACK_RING_RESULT_OK);
    }
    device_pcm_frame_view_t overflow = frame(payload, AUDIO_PLAYBACK_RING_FRAME_CAPACITY, 0);
    assert(audio_playback_ring_push(ring, TEST_CONNECTION_EPOCH, &overflow) ==
           AUDIO_PLAYBACK_RING_RESULT_OVERFLOW);
    device_pcm_frame_view_t stale = frame(payload, 0U, 0);
    assert(audio_playback_ring_push(ring, TEST_CONNECTION_EPOCH + 1U, &stale) ==
           AUDIO_PLAYBACK_RING_RESULT_STREAM_MISMATCH);
    assert(audio_playback_ring_end(ring, TEST_CONNECTION_EPOCH, TEST_STREAM_ID) ==
           AUDIO_PLAYBACK_RING_RESULT_INCOMPLETE);
    audio_playback_ring_destroy(ring);
}

int main(void)
{
    verify_sequence_and_tail();
    verify_overflow_and_isolation();
    puts("audio playback ring boundary and isolation checks passed");
    return 0;
}
