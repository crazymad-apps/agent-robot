#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_media_control.h"
#include "device_pcm_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct audio_io audio_io_t;

typedef enum {
    AUDIO_IO_RESULT_OK = 0,
    AUDIO_IO_RESULT_INVALID_ARGUMENT,
    AUDIO_IO_RESULT_NO_MEMORY,
    AUDIO_IO_RESULT_INVALID_STATE,
    AUDIO_IO_RESULT_BUSY,
    AUDIO_IO_RESULT_SESSION_MISMATCH,
    AUDIO_IO_RESULT_SEQUENCE_MISMATCH,
    AUDIO_IO_RESULT_BUFFER_OVERFLOW,
    AUDIO_IO_RESULT_SAMPLE_COUNT_EXCEEDED,
    AUDIO_IO_RESULT_INCOMPLETE,
    AUDIO_IO_RESULT_HARDWARE_FAILURE,
} audio_io_result_t;

typedef enum {
    AUDIO_IO_PLAYBACK_STATE_STOPPED = 0,
    AUDIO_IO_PLAYBACK_STATE_IDLE,
    AUDIO_IO_PLAYBACK_STATE_BUFFERING,
    AUDIO_IO_PLAYBACK_STATE_PLAYING,
} audio_io_playback_state_t;

typedef enum {
    AUDIO_IO_EVENT_PLAYBACK_STARTED = 0,
    AUDIO_IO_EVENT_PLAYBACK_COMPLETED,
    AUDIO_IO_EVENT_PLAYBACK_CANCELLED,
    AUDIO_IO_EVENT_PLAYBACK_FAILED,
} audio_io_event_type_t;

typedef struct {
    audio_io_event_type_t type;
    audio_io_result_t result;
    uint64_t connection_epoch;
    uint32_t stream_id;
    char output_id[DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES + 1U];
} audio_io_event_t;

/* Events run synchronously on the Audio task and the callback must not block. */
typedef void (*audio_io_event_callback_t)(const audio_io_event_t *event, void *context);

typedef struct {
    uint8_t playback_gain_percent;
    audio_io_event_callback_t event_callback;
    void *event_context;
} audio_io_config_t;

typedef struct {
    audio_io_playback_state_t playback_state;
    uint64_t connection_epoch;
    uint32_t stream_id;
    char output_id[DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES + 1U];
    uint64_t sample_count;
    uint64_t received_frame_count;
    uint64_t consumed_sample_count;
    size_t queued_frame_count;
    uint32_t completed_playback_count;
    uint32_t cancelled_playback_count;
    uint32_t failed_playback_count;
} audio_io_status_t;

audio_io_result_t audio_io_create(const audio_io_config_t *config, audio_io_t **audio_io);

audio_io_result_t audio_io_start(audio_io_t *audio_io);

/* These ingress functions are non-blocking and are safe to call on the Network task. */
audio_io_result_t audio_io_playback_begin(
    audio_io_t *audio_io,
    uint64_t connection_epoch,
    const device_media_playback_start_t *playback_start);

audio_io_result_t audio_io_playback_push(
    audio_io_t *audio_io,
    uint64_t connection_epoch,
    const device_pcm_frame_view_t *frame);

audio_io_result_t audio_io_playback_end(
    audio_io_t *audio_io,
    uint64_t connection_epoch,
    const device_media_playback_end_t *playback_end);

/* Cancels only the matching connection epoch; stale disconnects cannot stop a new session. */
audio_io_result_t audio_io_playback_cancel(audio_io_t *audio_io, uint64_t connection_epoch);

void audio_io_get_status(audio_io_t *audio_io, audio_io_status_t *status);

void audio_io_stop(audio_io_t *audio_io);

void audio_io_destroy(audio_io_t *audio_io);

#ifdef __cplusplus
}
#endif
