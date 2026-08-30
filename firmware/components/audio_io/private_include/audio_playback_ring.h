#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_pcm_frame.h"

#define AUDIO_PLAYBACK_RING_FRAME_CAPACITY 64U
#define AUDIO_PLAYBACK_RING_PREBUFFER_FRAMES 4U
#define AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME (DEVICE_PCM_PAYLOAD_BYTES / 2U)

typedef struct audio_playback_ring audio_playback_ring_t;

typedef enum {
    AUDIO_PLAYBACK_RING_RESULT_OK = 0,
    AUDIO_PLAYBACK_RING_RESULT_INVALID_ARGUMENT,
    AUDIO_PLAYBACK_RING_RESULT_NO_MEMORY,
    AUDIO_PLAYBACK_RING_RESULT_INVALID_STATE,
    AUDIO_PLAYBACK_RING_RESULT_STREAM_MISMATCH,
    AUDIO_PLAYBACK_RING_RESULT_SEQUENCE_MISMATCH,
    AUDIO_PLAYBACK_RING_RESULT_OVERFLOW,
    AUDIO_PLAYBACK_RING_RESULT_SAMPLE_COUNT_EXCEEDED,
    AUDIO_PLAYBACK_RING_RESULT_EMPTY,
    AUDIO_PLAYBACK_RING_RESULT_INCOMPLETE,
} audio_playback_ring_result_t;

typedef struct {
    uint64_t connection_epoch;
    uint32_t stream_id;
    uint64_t sample_count;
    uint64_t received_frame_count;
    uint64_t consumed_sample_count;
    size_t queued_frame_count;
    bool ended;
} audio_playback_ring_status_t;

audio_playback_ring_result_t audio_playback_ring_create(audio_playback_ring_t **ring);

audio_playback_ring_result_t audio_playback_ring_begin(audio_playback_ring_t *ring,
                                                       uint64_t connection_epoch,
                                                       uint32_t stream_id, uint64_t sample_count);

audio_playback_ring_result_t audio_playback_ring_push(audio_playback_ring_t *ring,
                                                      uint64_t connection_epoch,
                                                      const device_pcm_frame_view_t *frame);

audio_playback_ring_result_t audio_playback_ring_end(audio_playback_ring_t *ring,
                                                     uint64_t connection_epoch, uint32_t stream_id);

audio_playback_ring_result_t
audio_playback_ring_pop(audio_playback_ring_t *ring,
                        int16_t samples[AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME],
                        size_t *sample_count);

bool audio_playback_ring_ready(const audio_playback_ring_t *ring);

bool audio_playback_ring_drained(const audio_playback_ring_t *ring);

void audio_playback_ring_get_status(const audio_playback_ring_t *ring,
                                    audio_playback_ring_status_t *status);

void audio_playback_ring_clear(audio_playback_ring_t *ring);

void audio_playback_ring_destroy(audio_playback_ring_t *ring);
