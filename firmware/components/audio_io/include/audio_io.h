#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "device_media_control.h"
#include "device_pcm_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_IO_DEFAULT_PLAYBACK_GAIN_PERCENT 75U
#define AUDIO_IO_CAPTURE_SAMPLE_RATE_HZ 16000U
#define AUDIO_IO_CAPTURE_FRAME_DURATION_MS 20U
#define AUDIO_IO_CAPTURE_SAMPLES_PER_FRAME \
    (AUDIO_IO_CAPTURE_SAMPLE_RATE_HZ * AUDIO_IO_CAPTURE_FRAME_DURATION_MS / 1000U)

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
    AUDIO_IO_CAPTURE_STATE_STOPPED = 0,
    AUDIO_IO_CAPTURE_STATE_IDLE,
    AUDIO_IO_CAPTURE_STATE_STARTING,
    AUDIO_IO_CAPTURE_STATE_CAPTURING,
    AUDIO_IO_CAPTURE_STATE_STOPPING,
} audio_io_capture_state_t;

typedef enum {
    AUDIO_IO_EVENT_PLAYBACK_STARTED = 0,
    AUDIO_IO_EVENT_PLAYBACK_COMPLETED,
    AUDIO_IO_EVENT_PLAYBACK_CANCELLED,
    AUDIO_IO_EVENT_PLAYBACK_FAILED,
    AUDIO_IO_EVENT_CAPTURE_STARTED,
    AUDIO_IO_EVENT_CAPTURE_COMPLETED,
    AUDIO_IO_EVENT_CAPTURE_CANCELLED,
    AUDIO_IO_EVENT_CAPTURE_FAILED,
} audio_io_event_type_t;

typedef struct {
    audio_io_event_type_t type;
    audio_io_result_t result;
    uint64_t connection_epoch;
    uint32_t stream_id;
    char output_id[DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES + 1U];
    uint64_t capture_session_id;
    uint64_t capture_sample_count;
} audio_io_event_t;

/* Events run synchronously on the Audio task and the callback must not block. */
typedef void (*audio_io_event_callback_t)(const audio_io_event_t *event, void *context);

/* The samples are valid only for the callback duration; the callback must not block. */
typedef bool (*audio_io_capture_frame_callback_t)(
    uint64_t capture_session_id,
    const int16_t samples[AUDIO_IO_CAPTURE_SAMPLES_PER_FRAME],
    size_t sample_count,
    void *context);

typedef struct {
    uint8_t playback_gain_percent;
    audio_io_event_callback_t event_callback;
    void *event_context;
    audio_io_capture_frame_callback_t capture_frame_callback;
    void *capture_frame_context;
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
    size_t queued_playback_count;
    uint32_t completed_playback_count;
    uint32_t cancelled_playback_count;
    uint32_t failed_playback_count;
    audio_io_capture_state_t capture_state;
    uint64_t capture_session_id;
    uint64_t capture_sample_count;
    uint64_t capture_frame_count;
    uint32_t completed_capture_count;
    uint32_t cancelled_capture_count;
    uint32_t failed_capture_count;
} audio_io_status_t;

audio_io_result_t audio_io_create(const audio_io_config_t *config, audio_io_t **audio_io);

audio_io_result_t audio_io_start(audio_io_t *audio_io);

/* Capture and playback are half-duplex and share one Audio task/I2S owner. */
audio_io_result_t audio_io_capture_begin(
    audio_io_t *audio_io,
    uint64_t capture_session_id);

audio_io_result_t audio_io_capture_end(
    audio_io_t *audio_io,
    uint64_t capture_session_id);

audio_io_result_t audio_io_capture_cancel(
    audio_io_t *audio_io,
    uint64_t capture_session_id);

/* These ingress functions use only a bounded lock wait and perform no blocking I/O. */
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

/*
 * Cancels only the matching connection epoch; stale disconnects cannot stop a
 * new session. This lifecycle operation may wait for the Audio task lock.
 */
audio_io_result_t audio_io_playback_cancel(audio_io_t *audio_io, uint64_t connection_epoch);

void audio_io_get_status(audio_io_t *audio_io, audio_io_status_t *status);

void audio_io_stop(audio_io_t *audio_io);

void audio_io_destroy(audio_io_t *audio_io);

#ifdef __cplusplus
}
#endif
