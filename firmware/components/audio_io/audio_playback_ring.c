#include "audio_playback_ring.h"

#include <stdlib.h>
#include <string.h>

#include "audio_playback_ring_memory.h"

#define AUDIO_PLAYBACK_RING_STORAGE_BYTES                                                          \
    (AUDIO_PLAYBACK_RING_FRAME_CAPACITY * DEVICE_PCM_PAYLOAD_BYTES)

struct audio_playback_ring {
    uint8_t *storage;
    uint64_t connection_epoch;
    uint32_t stream_id;
    uint64_t sample_count;
    uint64_t required_frame_count;
    uint64_t received_frame_count;
    uint64_t consumed_sample_count;
    size_t head;
    size_t queued_frame_count;
    bool active;
    bool ended;
};

static void secure_zero(void *memory, size_t size)
{
    volatile uint8_t *bytes = memory;
    while (size > 0U) {
        *bytes++ = 0U;
        --size;
    }
}

static bool session_matches(const audio_playback_ring_t *ring, uint64_t connection_epoch,
                            uint32_t stream_id)
{
    return ring->active && ring->connection_epoch == connection_epoch &&
           ring->stream_id == stream_id;
}

static int16_t decode_pcm16_little_endian(const uint8_t *encoded)
{
    const uint16_t raw = (uint16_t)encoded[0] | ((uint16_t)encoded[1] << 8U);
    const int32_t signed_sample =
        raw >= UINT16_C(0x8000) ? (int32_t)raw - INT32_C(0x10000) : (int32_t)raw;
    return (int16_t)signed_sample;
}

audio_playback_ring_result_t audio_playback_ring_create(audio_playback_ring_t **ring)
{
    if (ring == NULL || *ring != NULL) {
        return AUDIO_PLAYBACK_RING_RESULT_INVALID_ARGUMENT;
    }
    audio_playback_ring_t *created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        return AUDIO_PLAYBACK_RING_RESULT_NO_MEMORY;
    }
    created->storage = audio_playback_ring_memory_allocate(AUDIO_PLAYBACK_RING_STORAGE_BYTES);
    if (created->storage == NULL) {
        free(created);
        return AUDIO_PLAYBACK_RING_RESULT_NO_MEMORY;
    }
    *ring = created;
    return AUDIO_PLAYBACK_RING_RESULT_OK;
}

audio_playback_ring_result_t audio_playback_ring_begin(audio_playback_ring_t *ring,
                                                       uint64_t connection_epoch,
                                                       uint32_t stream_id, uint64_t sample_count)
{
    if (ring == NULL || connection_epoch == 0U || stream_id == 0U || sample_count == 0U) {
        return AUDIO_PLAYBACK_RING_RESULT_INVALID_ARGUMENT;
    }
    if (ring->active) {
        return AUDIO_PLAYBACK_RING_RESULT_INVALID_STATE;
    }
    const uint64_t required_frame_count =
        sample_count / AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME +
        (sample_count % AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME == 0U ? 0U : 1U);
    if (required_frame_count > UINT32_MAX) {
        return AUDIO_PLAYBACK_RING_RESULT_INVALID_ARGUMENT;
    }
    ring->connection_epoch = connection_epoch;
    ring->stream_id = stream_id;
    ring->sample_count = sample_count;
    ring->required_frame_count = required_frame_count;
    ring->active = true;
    return AUDIO_PLAYBACK_RING_RESULT_OK;
}

audio_playback_ring_result_t audio_playback_ring_push(audio_playback_ring_t *ring,
                                                      uint64_t connection_epoch,
                                                      const device_pcm_frame_view_t *frame)
{
    if (ring == NULL || frame == NULL || frame->direction != DEVICE_PCM_DIRECTION_DOWNLINK ||
        frame->stream_id == 0U || frame->payload == NULL ||
        frame->payload_size != DEVICE_PCM_PAYLOAD_BYTES) {
        return AUDIO_PLAYBACK_RING_RESULT_INVALID_ARGUMENT;
    }
    if (!ring->active || ring->ended) {
        return AUDIO_PLAYBACK_RING_RESULT_INVALID_STATE;
    }
    if (!session_matches(ring, connection_epoch, frame->stream_id)) {
        return AUDIO_PLAYBACK_RING_RESULT_STREAM_MISMATCH;
    }
    if ((uint64_t)frame->sequence != ring->received_frame_count) {
        return AUDIO_PLAYBACK_RING_RESULT_SEQUENCE_MISMATCH;
    }
    if (ring->received_frame_count >= ring->required_frame_count) {
        return AUDIO_PLAYBACK_RING_RESULT_SAMPLE_COUNT_EXCEEDED;
    }
    if (ring->queued_frame_count == AUDIO_PLAYBACK_RING_FRAME_CAPACITY) {
        return AUDIO_PLAYBACK_RING_RESULT_OVERFLOW;
    }
    const size_t tail =
        (ring->head + ring->queued_frame_count) % AUDIO_PLAYBACK_RING_FRAME_CAPACITY;
    memcpy(&ring->storage[tail * DEVICE_PCM_PAYLOAD_BYTES], frame->payload,
           DEVICE_PCM_PAYLOAD_BYTES);
    ++ring->queued_frame_count;
    ++ring->received_frame_count;
    return AUDIO_PLAYBACK_RING_RESULT_OK;
}

audio_playback_ring_result_t audio_playback_ring_end(audio_playback_ring_t *ring,
                                                     uint64_t connection_epoch, uint32_t stream_id)
{
    if (ring == NULL || connection_epoch == 0U || stream_id == 0U) {
        return AUDIO_PLAYBACK_RING_RESULT_INVALID_ARGUMENT;
    }
    if (!ring->active || ring->ended) {
        return AUDIO_PLAYBACK_RING_RESULT_INVALID_STATE;
    }
    if (!session_matches(ring, connection_epoch, stream_id)) {
        return AUDIO_PLAYBACK_RING_RESULT_STREAM_MISMATCH;
    }
    if (ring->received_frame_count != ring->required_frame_count) {
        return AUDIO_PLAYBACK_RING_RESULT_INCOMPLETE;
    }
    ring->ended = true;
    return AUDIO_PLAYBACK_RING_RESULT_OK;
}

audio_playback_ring_result_t
audio_playback_ring_pop(audio_playback_ring_t *ring,
                        int16_t samples[AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME],
                        size_t *sample_count)
{
    if (ring == NULL || samples == NULL || sample_count == NULL) {
        return AUDIO_PLAYBACK_RING_RESULT_INVALID_ARGUMENT;
    }
    if (!ring->active) {
        return AUDIO_PLAYBACK_RING_RESULT_INVALID_STATE;
    }
    if (ring->queued_frame_count == 0U) {
        return AUDIO_PLAYBACK_RING_RESULT_EMPTY;
    }
    const uint8_t *encoded = &ring->storage[ring->head * DEVICE_PCM_PAYLOAD_BYTES];
    const uint64_t remaining_samples = ring->sample_count - ring->consumed_sample_count;
    const size_t valid_samples = remaining_samples < AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME
                                     ? (size_t)remaining_samples
                                     : AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME;
    for (size_t index = 0U; index < valid_samples; ++index) {
        samples[index] = decode_pcm16_little_endian(&encoded[index * 2U]);
    }
    if (valid_samples < AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME) {
        memset(&samples[valid_samples], 0,
               (AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME - valid_samples) * sizeof(samples[0]));
    }
    secure_zero(&ring->storage[ring->head * DEVICE_PCM_PAYLOAD_BYTES], DEVICE_PCM_PAYLOAD_BYTES);
    ring->head = (ring->head + 1U) % AUDIO_PLAYBACK_RING_FRAME_CAPACITY;
    --ring->queued_frame_count;
    ring->consumed_sample_count += valid_samples;
    *sample_count = valid_samples;
    return AUDIO_PLAYBACK_RING_RESULT_OK;
}

bool audio_playback_ring_ready(const audio_playback_ring_t *ring)
{
    return ring != NULL && ring->active &&
           (ring->queued_frame_count >= AUDIO_PLAYBACK_RING_PREBUFFER_FRAMES ||
            (ring->ended && ring->queued_frame_count > 0U));
}

bool audio_playback_ring_drained(const audio_playback_ring_t *ring)
{
    return ring != NULL && ring->active && ring->ended && ring->queued_frame_count == 0U &&
           ring->consumed_sample_count == ring->sample_count;
}

void audio_playback_ring_get_status(const audio_playback_ring_t *ring,
                                    audio_playback_ring_status_t *status)
{
    if (ring == NULL || status == NULL) {
        return;
    }
    *status = (audio_playback_ring_status_t){
        .connection_epoch = ring->connection_epoch,
        .stream_id = ring->stream_id,
        .sample_count = ring->sample_count,
        .received_frame_count = ring->received_frame_count,
        .consumed_sample_count = ring->consumed_sample_count,
        .queued_frame_count = ring->queued_frame_count,
        .ended = ring->ended,
    };
}

void audio_playback_ring_clear(audio_playback_ring_t *ring)
{
    if (ring == NULL) {
        return;
    }
    secure_zero(ring->storage, AUDIO_PLAYBACK_RING_STORAGE_BYTES);
    ring->connection_epoch = 0U;
    ring->stream_id = 0U;
    ring->sample_count = 0U;
    ring->required_frame_count = 0U;
    ring->received_frame_count = 0U;
    ring->consumed_sample_count = 0U;
    ring->head = 0U;
    ring->queued_frame_count = 0U;
    ring->active = false;
    ring->ended = false;
}

void audio_playback_ring_destroy(audio_playback_ring_t *ring)
{
    if (ring == NULL) {
        return;
    }
    audio_playback_ring_clear(ring);
    audio_playback_ring_memory_release(ring->storage);
    ring->storage = NULL;
    secure_zero(ring, sizeof(*ring));
    free(ring);
}
