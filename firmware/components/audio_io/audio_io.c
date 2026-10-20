#include "audio_io.h"

#include <stdlib.h>
#include <string.h>

#include "audio_hardware.h"
#include "audio_pcm_conversion.h"
#include "audio_playback_queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AUDIO_IO_TASK_STACK_BYTES 8192U
#define AUDIO_IO_TASK_PRIORITY 7U
#define AUDIO_IO_PLAYBACK_WRITE_TIMEOUT_MS 100U
#define AUDIO_IO_CAPTURE_READ_TIMEOUT_MS 100U
#define AUDIO_IO_PLAYBACK_PRELOAD_DURATION_US 20000U
#define AUDIO_IO_PLAYBACK_INITIAL_BUFFER_FRAMES 100U
#define AUDIO_IO_PLAYBACK_RECOVERY_BUFFER_FRAMES 100U
#define AUDIO_IO_STOPPED_BIT BIT0
#define AUDIO_IO_INGRESS_LOCK_WAIT_TICKS 1U

_Static_assert(
    AUDIO_IO_CAPTURE_SAMPLES_PER_FRAME == AUDIO_PLAYBACK_QUEUE_SAMPLES_PER_FRAME,
    "capture and playback must use the protocol PCM frame size");
_Static_assert(
    AUDIO_IO_PLAYBACK_INITIAL_BUFFER_FRAMES <= AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY,
    "initial playback buffer must fit in the playback queue");
_Static_assert(
    AUDIO_IO_PLAYBACK_RECOVERY_BUFFER_FRAMES <= AUDIO_PLAYBACK_QUEUE_FRAME_CAPACITY,
    "recovery playback buffer must fit in the playback queue");

struct audio_io {
    SemaphoreHandle_t lock;
    EventGroupHandle_t lifecycle_events;
    TaskHandle_t task;
    audio_hardware_t *hardware;
    audio_playback_queue_t *playback_queue;
    audio_io_event_callback_t event_callback;
    void *event_context;
    audio_io_capture_frame_callback_t capture_frame_callback;
    void *capture_frame_context;
    uint8_t playback_gain_percent;
    bool started;
    bool stop_requested;
    bool playback_active;
    bool hardware_playing;
    bool playback_started;
    bool playback_rebuffering;
    uint64_t playback_generation;
    int64_t playback_buffered_until_us;
    char output_id[DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES + 1U];
    bool cancellation_pending;
    audio_io_event_t cancellation_event;
    bool discarded_session_active;
    uint64_t discarded_connection_epoch;
    uint32_t discarded_stream_id;
    char discarded_output_id[DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES + 1U];
    bool capture_requested;
    bool capture_active;
    bool capture_stop_requested;
    bool capture_cancel_requested;
    bool hardware_capturing;
    uint64_t capture_generation;
    audio_io_status_t status;
};

static bool identifier_is_terminated(const char *identifier, size_t capacity)
{
    return identifier != NULL && identifier[0] != '\0' &&
           memchr(identifier, '\0', capacity) != NULL;
}

static void emit_event(audio_io_t *audio_io, const audio_io_event_t *event)
{
    if (audio_io->event_callback != NULL) {
        audio_io->event_callback(event, audio_io->event_context);
    }
}

static void copy_active_event(
    const audio_io_t *audio_io,
    audio_io_event_type_t type,
    audio_io_result_t result,
    audio_io_event_t *event)
{
    *event = (audio_io_event_t){
        .type = type,
        .result = result,
        .connection_epoch = audio_io->status.connection_epoch,
        .stream_id = audio_io->status.stream_id,
        .playback_rebuffer_count = audio_io->status.playback_rebuffer_count,
        .sample_count = audio_io->status.sample_count,
        .received_frame_count = audio_io->status.received_frame_count,
        .consumed_sample_count = audio_io->status.consumed_sample_count,
        .queued_frame_count = audio_io->status.queued_frame_count,
    };
    memcpy(event->output_id, audio_io->output_id, sizeof(event->output_id));
}

static void copy_capture_event(
    const audio_io_t *audio_io,
    audio_io_event_type_t type,
    audio_io_result_t result,
    audio_io_event_t *event)
{
    *event = (audio_io_event_t){
        .type = type,
        .result = result,
        .capture_session_id = audio_io->status.capture_session_id,
        .capture_sample_count = audio_io->status.capture_sample_count,
    };
}

static audio_io_result_t queue_result(audio_playback_queue_result_t result)
{
    switch (result) {
    case AUDIO_PLAYBACK_QUEUE_RESULT_OK:
        return AUDIO_IO_RESULT_OK;
    case AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_ARGUMENT:
        return AUDIO_IO_RESULT_INVALID_ARGUMENT;
    case AUDIO_PLAYBACK_QUEUE_RESULT_NO_MEMORY:
        return AUDIO_IO_RESULT_NO_MEMORY;
    case AUDIO_PLAYBACK_QUEUE_RESULT_INVALID_STATE:
    case AUDIO_PLAYBACK_QUEUE_RESULT_EMPTY:
        return AUDIO_IO_RESULT_INVALID_STATE;
    case AUDIO_PLAYBACK_QUEUE_RESULT_STREAM_MISMATCH:
        return AUDIO_IO_RESULT_SESSION_MISMATCH;
    case AUDIO_PLAYBACK_QUEUE_RESULT_SEQUENCE_MISMATCH:
        return AUDIO_IO_RESULT_SEQUENCE_MISMATCH;
    case AUDIO_PLAYBACK_QUEUE_RESULT_FRAME_CAPACITY_EXCEEDED:
    case AUDIO_PLAYBACK_QUEUE_RESULT_STREAM_CAPACITY_EXCEEDED:
        return AUDIO_IO_RESULT_BUFFER_OVERFLOW;
    case AUDIO_PLAYBACK_QUEUE_RESULT_SAMPLE_COUNT_EXCEEDED:
        return AUDIO_IO_RESULT_SAMPLE_COUNT_EXCEEDED;
    case AUDIO_PLAYBACK_QUEUE_RESULT_INCOMPLETE:
        return AUDIO_IO_RESULT_INCOMPLETE;
    default:
        return AUDIO_IO_RESULT_INVALID_STATE;
    }
}

static void update_playback_status(audio_io_t *audio_io);

static void reset_active_playback(audio_io_t *audio_io)
{
    audio_io->playback_active = false;
    audio_io->playback_started = false;
    audio_io->playback_rebuffering = false;
    ++audio_io->playback_generation;
    audio_io->playback_buffered_until_us = 0;
    memset(audio_io->output_id, 0, sizeof(audio_io->output_id));
    audio_io->status.playback_state = audio_io->started ? AUDIO_IO_PLAYBACK_STATE_IDLE
                                                       : AUDIO_IO_PLAYBACK_STATE_STOPPED;
    audio_io->status.connection_epoch = 0U;
    audio_io->status.stream_id = 0U;
    memset(audio_io->status.output_id, 0, sizeof(audio_io->status.output_id));
    audio_io->status.sample_count = 0U;
    audio_io->status.received_frame_count = 0U;
    audio_io->status.consumed_sample_count = 0U;
    audio_io->status.queued_frame_count = 0U;
    audio_io->status.queued_playback_count = 0U;
    audio_io->status.playback_rebuffer_count = 0U;
}

static void clear_playback_queue(audio_io_t *audio_io)
{
    audio_playback_queue_clear(audio_io->playback_queue);
    reset_active_playback(audio_io);
}

static void clear_capture(audio_io_t *audio_io)
{
    audio_io->capture_requested = false;
    audio_io->capture_active = false;
    audio_io->capture_stop_requested = false;
    audio_io->capture_cancel_requested = false;
    ++audio_io->capture_generation;
    audio_io->status.capture_state = audio_io->started ? AUDIO_IO_CAPTURE_STATE_IDLE
                                                       : AUDIO_IO_CAPTURE_STATE_STOPPED;
    audio_io->status.capture_session_id = 0U;
    audio_io->status.capture_sample_count = 0U;
    audio_io->status.capture_frame_count = 0U;
}

static bool activate_playback_head(audio_io_t *audio_io)
{
    audio_playback_queue_status_t queue_status = {0};
    audio_playback_queue_get_status(audio_io->playback_queue, &queue_status);
    if (queue_status.stream_count == 0U) {
        return false;
    }
    audio_io->playback_active = true;
    audio_io->playback_started = false;
    audio_io->playback_rebuffering = false;
    ++audio_io->playback_generation;
    audio_io->playback_buffered_until_us = 0;
    memcpy(audio_io->output_id, queue_status.output_id, sizeof(audio_io->output_id));
    audio_io->status.playback_state = AUDIO_IO_PLAYBACK_STATE_BUFFERING;
    audio_io->status.playback_rebuffer_count = 0U;
    update_playback_status(audio_io);
    return true;
}

static uint64_t sample_duration_us(uint64_t sample_count)
{
    const uint64_t seconds = sample_count / 16000U;
    const uint64_t remaining_samples = sample_count % 16000U;
    return seconds * 1000000U + (remaining_samples * 1000000U) / 16000U;
}

static int64_t playback_buffered_until_us(uint64_t buffered_duration_us)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint64_t buffered_until_us =
        buffered_duration_us > UINT64_MAX - now_us
            ? UINT64_MAX
            : now_us + buffered_duration_us;
    return buffered_until_us > INT64_MAX ? INT64_MAX : (int64_t)buffered_until_us;
}

static void update_playback_status(audio_io_t *audio_io)
{
    audio_playback_queue_status_t queue_status = {0};
    audio_playback_queue_get_status(audio_io->playback_queue, &queue_status);
    audio_io->status.connection_epoch = queue_status.connection_epoch;
    audio_io->status.stream_id = queue_status.stream_id;
    memcpy(audio_io->status.output_id, queue_status.output_id,
           sizeof(audio_io->status.output_id));
    audio_io->status.sample_count = queue_status.sample_count;
    audio_io->status.received_frame_count = queue_status.received_frame_count;
    audio_io->status.consumed_sample_count = queue_status.consumed_sample_count;
    audio_io->status.queued_frame_count = queue_status.queued_frame_count;
    audio_io->status.queued_playback_count =
        queue_status.stream_count == 0U ? 0U : queue_status.stream_count - 1U;
}

static void record_event_count(audio_io_t *audio_io, audio_io_event_type_t type)
{
    if (type == AUDIO_IO_EVENT_PLAYBACK_COMPLETED) {
        ++audio_io->status.completed_playback_count;
    } else if (type == AUDIO_IO_EVENT_PLAYBACK_CANCELLED) {
        ++audio_io->status.cancelled_playback_count;
    } else if (type == AUDIO_IO_EVENT_PLAYBACK_FAILED) {
        ++audio_io->status.failed_playback_count;
    } else if (type == AUDIO_IO_EVENT_CAPTURE_COMPLETED) {
        ++audio_io->status.completed_capture_count;
    } else if (type == AUDIO_IO_EVENT_CAPTURE_CANCELLED) {
        ++audio_io->status.cancelled_capture_count;
    } else if (type == AUDIO_IO_EVENT_CAPTURE_FAILED) {
        ++audio_io->status.failed_capture_count;
    }
}

static void playback_finish(
    audio_io_t *audio_io,
    uint64_t generation,
    audio_io_event_type_t type,
    audio_io_result_t result,
    bool hardware_started)
{
    audio_io_event_t event = {0};
    bool event_ready = false;
    audio_hardware_result_t stop_result = AUDIO_HARDWARE_RESULT_OK;
    (void)audio_hardware_playback_mute(audio_io->hardware);
    if (hardware_started) {
        stop_result = audio_hardware_playback_stop(audio_io->hardware);
    }
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    audio_io->hardware_playing = false;
    if (!audio_io->stop_requested && audio_io->playback_active &&
        audio_io->playback_generation == generation) {
        if (stop_result != AUDIO_HARDWARE_RESULT_OK) {
            type = AUDIO_IO_EVENT_PLAYBACK_FAILED;
            result = AUDIO_IO_RESULT_HARDWARE_FAILURE;
        }
        copy_active_event(audio_io, type, result, &event);
        if (type == AUDIO_IO_EVENT_PLAYBACK_COMPLETED) {
            const audio_playback_queue_result_t complete_result =
                audio_playback_queue_complete_head(audio_io->playback_queue);
            reset_active_playback(audio_io);
            if (complete_result == AUDIO_PLAYBACK_QUEUE_RESULT_OK) {
                (void)activate_playback_head(audio_io);
            } else {
                type = AUDIO_IO_EVENT_PLAYBACK_FAILED;
                event.type = type;
                event.result = queue_result(complete_result);
                clear_playback_queue(audio_io);
            }
        } else {
            clear_playback_queue(audio_io);
        }
        record_event_count(audio_io, type);
        event_ready = true;
    }
    xSemaphoreGive(audio_io->lock);
    if (event_ready) {
        emit_event(audio_io, &event);
    }
}

static void process_pending_cancellation(audio_io_t *audio_io)
{
    audio_io_event_t event;
    bool hardware_started;
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    event = audio_io->cancellation_event;
    audio_io->cancellation_pending = false;
    memset(&audio_io->cancellation_event, 0, sizeof(audio_io->cancellation_event));
    hardware_started = audio_io->hardware_playing;
    xSemaphoreGive(audio_io->lock);

    audio_hardware_result_t stop_result = AUDIO_HARDWARE_RESULT_OK;
    if (hardware_started) {
        stop_result = audio_hardware_playback_stop(audio_io->hardware);
    }
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    audio_io->hardware_playing = false;
    if (stop_result != AUDIO_HARDWARE_RESULT_OK) {
        event.type = AUDIO_IO_EVENT_PLAYBACK_FAILED;
        event.result = AUDIO_IO_RESULT_HARDWARE_FAILURE;
    }
    record_event_count(audio_io, event.type);
    xSemaphoreGive(audio_io->lock);
    emit_event(audio_io, &event);
}

static void capture_finish(
    audio_io_t *audio_io,
    uint64_t generation,
    audio_io_event_type_t type,
    audio_io_result_t result,
    bool hardware_started)
{
    audio_hardware_result_t stop_result = AUDIO_HARDWARE_RESULT_OK;
    if (hardware_started) {
        stop_result = audio_hardware_capture_stop(audio_io->hardware);
    }
    audio_io_event_t event = {0};
    bool event_ready = false;
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    audio_io->hardware_capturing = false;
    if (!audio_io->stop_requested &&
        (audio_io->capture_requested || audio_io->capture_active) &&
        audio_io->capture_generation == generation) {
        if (stop_result != AUDIO_HARDWARE_RESULT_OK) {
            type = AUDIO_IO_EVENT_CAPTURE_FAILED;
            result = AUDIO_IO_RESULT_HARDWARE_FAILURE;
        }
        copy_capture_event(audio_io, type, result, &event);
        clear_capture(audio_io);
        record_event_count(audio_io, type);
        event_ready = true;
    }
    xSemaphoreGive(audio_io->lock);
    if (event_ready) {
        emit_event(audio_io, &event);
    }
}

static void capture_start(audio_io_t *audio_io, uint64_t generation)
{
    const audio_hardware_result_t start_result =
        audio_hardware_capture_start(audio_io->hardware);
    if (start_result != AUDIO_HARDWARE_RESULT_OK) {
        capture_finish(
            audio_io,
            generation,
            AUDIO_IO_EVENT_CAPTURE_FAILED,
            AUDIO_IO_RESULT_HARDWARE_FAILURE,
            false);
        return;
    }

    audio_io_event_t event = {0};
    bool session_still_active = false;
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    if (!audio_io->stop_requested && audio_io->capture_requested &&
        audio_io->capture_generation == generation) {
        audio_io->capture_requested = false;
        audio_io->capture_active = true;
        audio_io->hardware_capturing = true;
        audio_io->status.capture_state = AUDIO_IO_CAPTURE_STATE_CAPTURING;
        copy_capture_event(
            audio_io,
            AUDIO_IO_EVENT_CAPTURE_STARTED,
            AUDIO_IO_RESULT_OK,
            &event);
        session_still_active = true;
    }
    xSemaphoreGive(audio_io->lock);
    if (!session_still_active) {
        (void)audio_hardware_capture_stop(audio_io->hardware);
        return;
    }
    emit_event(audio_io, &event);
}

static void audio_io_task(void *context)
{
    audio_io_t *audio_io = context;
    int16_t pcm_samples[AUDIO_IO_CAPTURE_SAMPLES_PER_FRAME];
    int32_t i2s_words[AUDIO_IO_CAPTURE_SAMPLES_PER_FRAME * AUDIO_PCM_I2S_WORDS_PER_FRAME];
    while (true) {
        xSemaphoreTake(audio_io->lock, portMAX_DELAY);
        if (audio_io->stop_requested) {
            xSemaphoreGive(audio_io->lock);
            break;
        }
        if (audio_io->cancellation_pending) {
            xSemaphoreGive(audio_io->lock);
            process_pending_cancellation(audio_io);
            continue;
        }
        if (audio_io->capture_requested) {
            const uint64_t generation = audio_io->capture_generation;
            xSemaphoreGive(audio_io->lock);
            capture_start(audio_io, generation);
            continue;
        }
        if (audio_io->capture_active) {
            const uint64_t generation = audio_io->capture_generation;
            const uint64_t capture_session_id = audio_io->status.capture_session_id;
            const bool cancel_requested = audio_io->capture_cancel_requested;
            const bool stop_requested = audio_io->capture_stop_requested &&
                                        audio_io->status.capture_frame_count != 0U;
            if (cancel_requested || stop_requested) {
                const audio_io_event_type_t event_type =
                    cancel_requested ? AUDIO_IO_EVENT_CAPTURE_CANCELLED
                                     : AUDIO_IO_EVENT_CAPTURE_COMPLETED;
                xSemaphoreGive(audio_io->lock);
                capture_finish(
                    audio_io,
                    generation,
                    event_type,
                    AUDIO_IO_RESULT_OK,
                    true);
                continue;
            }
            xSemaphoreGive(audio_io->lock);

            size_t i2s_word_count = 0U;
            const audio_hardware_result_t read_result = audio_hardware_capture_read(
                audio_io->hardware,
                i2s_words,
                sizeof(i2s_words) / sizeof(i2s_words[0]),
                &i2s_word_count,
                AUDIO_IO_CAPTURE_READ_TIMEOUT_MS);
            size_t pcm_sample_count = 0U;
            const audio_pcm_conversion_result_t conversion_result =
                read_result == AUDIO_HARDWARE_RESULT_OK
                    ? audio_pcm_capture_convert(
                          i2s_words,
                          i2s_word_count,
                          pcm_samples,
                          sizeof(pcm_samples) / sizeof(pcm_samples[0]),
                          &pcm_sample_count)
                    : AUDIO_PCM_CONVERSION_RESULT_INVALID_ARGUMENT;
            if (read_result != AUDIO_HARDWARE_RESULT_OK ||
                conversion_result != AUDIO_PCM_CONVERSION_RESULT_OK ||
                pcm_sample_count != AUDIO_IO_CAPTURE_SAMPLES_PER_FRAME) {
                capture_finish(
                    audio_io,
                    generation,
                    AUDIO_IO_EVENT_CAPTURE_FAILED,
                    AUDIO_IO_RESULT_HARDWARE_FAILURE,
                    true);
                continue;
            }
            const bool frame_accepted =
                audio_io->capture_frame_callback != NULL &&
                audio_io->capture_frame_callback(
                    capture_session_id,
                    pcm_samples,
                    pcm_sample_count,
                    audio_io->capture_frame_context);
            if (!frame_accepted) {
                capture_finish(
                    audio_io,
                    generation,
                    AUDIO_IO_EVENT_CAPTURE_FAILED,
                    AUDIO_IO_RESULT_BUFFER_OVERFLOW,
                    true);
                continue;
            }
            xSemaphoreTake(audio_io->lock, portMAX_DELAY);
            if (audio_io->capture_active &&
                audio_io->capture_generation == generation &&
                audio_io->status.capture_session_id == capture_session_id) {
                audio_io->status.capture_sample_count += pcm_sample_count;
                ++audio_io->status.capture_frame_count;
            }
            xSemaphoreGive(audio_io->lock);
            continue;
        }
        if (!audio_io->playback_active) {
            xSemaphoreGive(audio_io->lock);
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        const uint64_t generation = audio_io->playback_generation;
        /*
         * End may arrive after the last sample drained and underrun stopped I2S.
         * Settle the head before waiting for prebuffer: a completed empty head
         * can never become ready again, even when later streams are queued.
         */
        if (audio_playback_queue_head_drained(audio_io->playback_queue)) {
            const int64_t now_us = esp_timer_get_time();
            if (audio_io->hardware_playing && audio_io->playback_buffered_until_us > now_us) {
                const uint64_t remaining_us =
                    (uint64_t)(audio_io->playback_buffered_until_us - now_us);
                const uint64_t remaining_ms = (remaining_us + 999U) / 1000U;
                const TickType_t wait_ticks = pdMS_TO_TICKS(
                    remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_ms);
                xSemaphoreGive(audio_io->lock);
                (void)ulTaskNotifyTake(pdTRUE, wait_ticks == 0U ? 1U : wait_ticks);
                continue;
            }
            const bool hardware_playing = audio_io->hardware_playing;
            xSemaphoreGive(audio_io->lock);
            playback_finish(
                audio_io,
                generation,
                AUDIO_IO_EVENT_PLAYBACK_COMPLETED,
                AUDIO_IO_RESULT_OK,
                hardware_playing);
            continue;
        }
        if (!audio_io->hardware_playing) {
            const size_t prebuffer_frame_count =
                audio_io->playback_rebuffering
                    ? AUDIO_IO_PLAYBACK_RECOVERY_BUFFER_FRAMES
                    : AUDIO_IO_PLAYBACK_INITIAL_BUFFER_FRAMES;
            if (!audio_playback_queue_head_ready(
                    audio_io->playback_queue,
                    prebuffer_frame_count)) {
                xSemaphoreGive(audio_io->lock);
                (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }
            xSemaphoreGive(audio_io->lock);
            if (audio_hardware_playback_start(audio_io->hardware) != AUDIO_HARDWARE_RESULT_OK) {
                playback_finish(
                    audio_io,
                    generation,
                    AUDIO_IO_EVENT_PLAYBACK_FAILED,
                    AUDIO_IO_RESULT_HARDWARE_FAILURE,
                    false);
                continue;
            }
            audio_io_event_t started_event = {0};
            bool session_still_active = false;
            bool emit_started_event = false;
            xSemaphoreTake(audio_io->lock, portMAX_DELAY);
            if (!audio_io->stop_requested && audio_io->playback_active &&
                audio_io->playback_generation == generation) {
                audio_io->hardware_playing = true;
                if (audio_hardware_playback_unmute(audio_io->hardware) ==
                    AUDIO_HARDWARE_RESULT_OK) {
                    audio_io->status.playback_state = AUDIO_IO_PLAYBACK_STATE_PLAYING;
                    audio_io->playback_buffered_until_us =
                        playback_buffered_until_us(
                            AUDIO_IO_PLAYBACK_PRELOAD_DURATION_US);
                    audio_io->playback_rebuffering = false;
                    if (!audio_io->playback_started) {
                        audio_io->playback_started = true;
                        copy_active_event(
                            audio_io,
                            AUDIO_IO_EVENT_PLAYBACK_STARTED,
                            AUDIO_IO_RESULT_OK,
                            &started_event);
                        emit_started_event = true;
                    }
                    session_still_active = true;
                }
            }
            xSemaphoreGive(audio_io->lock);
            if (!session_still_active) {
                playback_finish(
                    audio_io,
                    generation,
                    AUDIO_IO_EVENT_PLAYBACK_FAILED,
                    AUDIO_IO_RESULT_HARDWARE_FAILURE,
                    true);
                continue;
            }
            if (emit_started_event) {
                emit_event(audio_io, &started_event);
            }
            continue;
        }
        const int64_t now_us = esp_timer_get_time();
        if (audio_io->playback_buffered_until_us <= now_us) {
            xSemaphoreGive(audio_io->lock);
            const audio_hardware_result_t mute_result =
                audio_hardware_playback_mute(audio_io->hardware);
            const audio_hardware_result_t stop_result =
                audio_hardware_playback_stop(audio_io->hardware);
            bool recovery_failed = false;
            audio_io_status_t recovery_status = {0};
            xSemaphoreTake(audio_io->lock, portMAX_DELAY);
            if (stop_result == AUDIO_HARDWARE_RESULT_OK) {
                audio_io->hardware_playing = false;
            }
            if (audio_io->playback_active &&
                audio_io->playback_generation == generation) {
                audio_io->playback_buffered_until_us = 0;
                if (mute_result == AUDIO_HARDWARE_RESULT_OK &&
                    stop_result == AUDIO_HARDWARE_RESULT_OK) {
                    audio_io->playback_rebuffering = true;
                    audio_io->status.playback_state = AUDIO_IO_PLAYBACK_STATE_BUFFERING;
                    ++audio_io->status.playback_rebuffer_count;
                    ++audio_io->status.total_playback_rebuffer_count;
                    recovery_status = audio_io->status;
                } else {
                    recovery_failed = true;
                }
            }
            xSemaphoreGive(audio_io->lock);
            if (recovery_status.playback_rebuffer_count != 0U) {
                ESP_LOGW("audio_io",
                         "event=playback_rebuffer stream_id=%lu connection_epoch=%llu "
                         "received_frames=%llu consumed_samples=%llu sample_count=%llu "
                         "queued_frames=%u count=%lu",
                         (unsigned long)recovery_status.stream_id,
                         (unsigned long long)recovery_status.connection_epoch,
                         (unsigned long long)recovery_status.received_frame_count,
                         (unsigned long long)recovery_status.consumed_sample_count,
                         (unsigned long long)recovery_status.sample_count,
                         (unsigned int)recovery_status.queued_frame_count,
                         (unsigned long)recovery_status.playback_rebuffer_count);
            }
            if (recovery_failed) {
                playback_finish(
                    audio_io,
                    generation,
                    AUDIO_IO_EVENT_PLAYBACK_FAILED,
                    AUDIO_IO_RESULT_HARDWARE_FAILURE,
                    stop_result != AUDIO_HARDWARE_RESULT_OK);
            }
            continue;
        }
        size_t pcm_sample_count = 0U;
        const audio_playback_queue_result_t pop_result =
            audio_playback_queue_pop(audio_io->playback_queue, pcm_samples, &pcm_sample_count);
        update_playback_status(audio_io);
        const int64_t buffered_until_us = audio_io->playback_buffered_until_us;
        xSemaphoreGive(audio_io->lock);
        if (pop_result == AUDIO_PLAYBACK_QUEUE_RESULT_EMPTY) {
            const uint64_t remaining_us =
                (uint64_t)(buffered_until_us - now_us);
            const uint64_t remaining_ms = (remaining_us + 999U) / 1000U;
            const TickType_t wait_ticks = pdMS_TO_TICKS(
                remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_ms);
            (void)ulTaskNotifyTake(pdTRUE, wait_ticks == 0U ? 1U : wait_ticks);
            continue;
        }
        if (pop_result != AUDIO_PLAYBACK_QUEUE_RESULT_OK) {
            playback_finish(
                audio_io,
                generation,
                AUDIO_IO_EVENT_PLAYBACK_FAILED,
                queue_result(pop_result),
                true);
            continue;
        }
        size_t i2s_word_count = 0U;
        const audio_pcm_conversion_result_t conversion_result = audio_pcm_playback_convert(
            pcm_samples,
            pcm_sample_count,
            audio_io->playback_gain_percent,
            i2s_words,
            sizeof(i2s_words) / sizeof(i2s_words[0]),
            &i2s_word_count);
        const audio_hardware_result_t write_result =
            conversion_result == AUDIO_PCM_CONVERSION_RESULT_OK
                ? audio_hardware_playback_write(
                      audio_io->hardware,
                      i2s_words,
                      i2s_word_count,
                      AUDIO_IO_PLAYBACK_WRITE_TIMEOUT_MS)
                : AUDIO_HARDWARE_RESULT_DRIVER_FAILURE;
        if (write_result != AUDIO_HARDWARE_RESULT_OK) {
            playback_finish(
                audio_io,
                generation,
                AUDIO_IO_EVENT_PLAYBACK_FAILED,
                AUDIO_IO_RESULT_HARDWARE_FAILURE,
                true);
            continue;
        }
        const uint64_t frame_duration_us = sample_duration_us(pcm_sample_count);
        xSemaphoreTake(audio_io->lock, portMAX_DELAY);
        if (audio_io->playback_active &&
            audio_io->playback_generation == generation) {
            const uint64_t current_time_us = (uint64_t)esp_timer_get_time();
            const uint64_t buffered_until_us =
                audio_io->playback_buffered_until_us > (int64_t)current_time_us
                    ? (uint64_t)audio_io->playback_buffered_until_us
                    : current_time_us;
            const uint64_t extended_buffered_until_us =
                frame_duration_us > UINT64_MAX - buffered_until_us
                    ? UINT64_MAX
                    : buffered_until_us + frame_duration_us;
            audio_io->playback_buffered_until_us =
                extended_buffered_until_us > INT64_MAX
                    ? INT64_MAX
                    : (int64_t)extended_buffered_until_us;
        }
        xSemaphoreGive(audio_io->lock);
    }

    (void)audio_hardware_playback_mute(audio_io->hardware);
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    const bool playback_hardware_started = audio_io->hardware_playing;
    const bool capture_hardware_started = audio_io->hardware_capturing;
    xSemaphoreGive(audio_io->lock);
    if (playback_hardware_started) {
        (void)audio_hardware_playback_stop(audio_io->hardware);
    }
    if (capture_hardware_started) {
        (void)audio_hardware_capture_stop(audio_io->hardware);
    }
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    audio_io->hardware_playing = false;
    audio_io->hardware_capturing = false;
    clear_playback_queue(audio_io);
    clear_capture(audio_io);
    audio_io->started = false;
    audio_io->stop_requested = false;
    audio_io->task = NULL;
    audio_io->status.playback_state = AUDIO_IO_PLAYBACK_STATE_STOPPED;
    audio_io->status.capture_state = AUDIO_IO_CAPTURE_STATE_STOPPED;
    xSemaphoreGive(audio_io->lock);
    memset(pcm_samples, 0, sizeof(pcm_samples));
    memset(i2s_words, 0, sizeof(i2s_words));
    xEventGroupSetBits(audio_io->lifecycle_events, AUDIO_IO_STOPPED_BIT);
    vTaskDelete(NULL);
}

audio_io_result_t audio_io_create(const audio_io_config_t *config, audio_io_t **audio_io)
{
    if (config == NULL || audio_io == NULL || *audio_io != NULL ||
        config->playback_gain_percent > 100U) {
        return AUDIO_IO_RESULT_INVALID_ARGUMENT;
    }
    audio_io_t *created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        return AUDIO_IO_RESULT_NO_MEMORY;
    }
    created->lock = xSemaphoreCreateMutex();
    created->lifecycle_events = xEventGroupCreate();
    if (created->lock == NULL || created->lifecycle_events == NULL) {
        if (created->lifecycle_events != NULL) {
            vEventGroupDelete(created->lifecycle_events);
        }
        if (created->lock != NULL) {
            vSemaphoreDelete(created->lock);
        }
        free(created);
        return AUDIO_IO_RESULT_NO_MEMORY;
    }
    const audio_playback_queue_result_t queue_create_result =
        audio_playback_queue_create(&created->playback_queue);
    if (queue_create_result != AUDIO_PLAYBACK_QUEUE_RESULT_OK) {
        vEventGroupDelete(created->lifecycle_events);
        vSemaphoreDelete(created->lock);
        free(created);
        return queue_result(queue_create_result);
    }
    if (audio_hardware_create(&created->hardware) != AUDIO_HARDWARE_RESULT_OK) {
        audio_hardware_destroy(created->hardware);
        audio_playback_queue_destroy(created->playback_queue);
        vEventGroupDelete(created->lifecycle_events);
        vSemaphoreDelete(created->lock);
        free(created);
        return AUDIO_IO_RESULT_HARDWARE_FAILURE;
    }
    created->event_callback = config->event_callback;
    created->event_context = config->event_context;
    created->capture_frame_callback = config->capture_frame_callback;
    created->capture_frame_context = config->capture_frame_context;
    created->playback_gain_percent = config->playback_gain_percent;
    created->status.playback_state = AUDIO_IO_PLAYBACK_STATE_STOPPED;
    created->status.capture_state = AUDIO_IO_CAPTURE_STATE_STOPPED;
    *audio_io = created;
    return AUDIO_IO_RESULT_OK;
}

audio_io_result_t audio_io_start(audio_io_t *audio_io)
{
    if (audio_io == NULL) {
        return AUDIO_IO_RESULT_INVALID_ARGUMENT;
    }
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    if (audio_io->started) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_INVALID_STATE;
    }
    audio_io->started = true;
    audio_io->stop_requested = false;
    audio_io->status.playback_state = AUDIO_IO_PLAYBACK_STATE_IDLE;
    audio_io->status.capture_state = AUDIO_IO_CAPTURE_STATE_IDLE;
    xEventGroupClearBits(audio_io->lifecycle_events, AUDIO_IO_STOPPED_BIT);
    const BaseType_t task_result = xTaskCreate(
        audio_io_task,
        "audio_io",
        AUDIO_IO_TASK_STACK_BYTES,
        audio_io,
        AUDIO_IO_TASK_PRIORITY,
        &audio_io->task);
    if (task_result != pdPASS) {
        audio_io->started = false;
        audio_io->status.playback_state = AUDIO_IO_PLAYBACK_STATE_STOPPED;
        audio_io->status.capture_state = AUDIO_IO_CAPTURE_STATE_STOPPED;
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_NO_MEMORY;
    }
    xSemaphoreGive(audio_io->lock);
    return AUDIO_IO_RESULT_OK;
}

audio_io_result_t audio_io_capture_begin(
    audio_io_t *audio_io,
    uint64_t capture_session_id)
{
    if (audio_io == NULL || capture_session_id == 0U) {
        return AUDIO_IO_RESULT_INVALID_ARGUMENT;
    }
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    if (!audio_io->started || audio_io->capture_frame_callback == NULL) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_INVALID_STATE;
    }
    if (audio_io->capture_requested || audio_io->capture_active ||
        audio_io->playback_active ||
        (audio_io->hardware_playing && !audio_io->cancellation_pending)) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_BUSY;
    }
    audio_io->capture_requested = true;
    audio_io->capture_stop_requested = false;
    audio_io->capture_cancel_requested = false;
    ++audio_io->capture_generation;
    audio_io->status.capture_state = AUDIO_IO_CAPTURE_STATE_STARTING;
    audio_io->status.capture_session_id = capture_session_id;
    audio_io->status.capture_sample_count = 0U;
    audio_io->status.capture_frame_count = 0U;
    TaskHandle_t task = audio_io->task;
    xSemaphoreGive(audio_io->lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    return AUDIO_IO_RESULT_OK;
}

audio_io_result_t audio_io_capture_end(
    audio_io_t *audio_io,
    uint64_t capture_session_id)
{
    if (audio_io == NULL || capture_session_id == 0U) {
        return AUDIO_IO_RESULT_INVALID_ARGUMENT;
    }
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    if ((!audio_io->capture_requested && !audio_io->capture_active) ||
        audio_io->status.capture_session_id != capture_session_id) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_SESSION_MISMATCH;
    }
    audio_io->capture_stop_requested = true;
    audio_io->status.capture_state = AUDIO_IO_CAPTURE_STATE_STOPPING;
    TaskHandle_t task = audio_io->task;
    xSemaphoreGive(audio_io->lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    return AUDIO_IO_RESULT_OK;
}

audio_io_result_t audio_io_capture_cancel(
    audio_io_t *audio_io,
    uint64_t capture_session_id)
{
    if (audio_io == NULL || capture_session_id == 0U) {
        return AUDIO_IO_RESULT_INVALID_ARGUMENT;
    }
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    if ((!audio_io->capture_requested && !audio_io->capture_active) ||
        audio_io->status.capture_session_id != capture_session_id) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_SESSION_MISMATCH;
    }
    audio_io->capture_cancel_requested = true;
    audio_io->status.capture_state = AUDIO_IO_CAPTURE_STATE_STOPPING;
    TaskHandle_t task = audio_io->task;
    xSemaphoreGive(audio_io->lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    return AUDIO_IO_RESULT_OK;
}

audio_io_result_t audio_io_playback_begin(
    audio_io_t *audio_io,
    uint64_t connection_epoch,
    const device_media_playback_start_t *playback_start)
{
    if (audio_io == NULL || connection_epoch == 0U || playback_start == NULL ||
        playback_start->stream_id == 0U || playback_start->sample_count == 0U ||
        !identifier_is_terminated(playback_start->output_id, sizeof(playback_start->output_id))) {
        return AUDIO_IO_RESULT_INVALID_ARGUMENT;
    }
    if (xSemaphoreTake(audio_io->lock, AUDIO_IO_INGRESS_LOCK_WAIT_TICKS) != pdTRUE) {
        return AUDIO_IO_RESULT_BUSY;
    }
    if (!audio_io->started) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_INVALID_STATE;
    }
    if (audio_io->capture_requested || audio_io->capture_active ||
        audio_io->hardware_capturing) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_BUSY;
    }
    if (audio_io->discarded_session_active &&
        audio_io->discarded_connection_epoch == connection_epoch &&
        audio_io->discarded_stream_id == playback_start->stream_id) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_INVALID_STATE;
    }
    if (audio_io->discarded_session_active &&
        audio_io->discarded_connection_epoch != connection_epoch) {
        audio_io->discarded_session_active = false;
        memset(audio_io->discarded_output_id, 0, sizeof(audio_io->discarded_output_id));
    }
    if (!audio_io->playback_active &&
        (audio_io->cancellation_pending || audio_io->hardware_playing)) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_BUSY;
    }
    const audio_io_result_t result = queue_result(audio_playback_queue_begin(
        audio_io->playback_queue,
        connection_epoch,
        playback_start));
    if (result == AUDIO_IO_RESULT_OK) {
        if (!audio_io->playback_active) {
            (void)activate_playback_head(audio_io);
        } else {
            update_playback_status(audio_io);
        }
    }
    TaskHandle_t task = audio_io->task;
    xSemaphoreGive(audio_io->lock);
    if (result == AUDIO_IO_RESULT_OK && task != NULL) {
        xTaskNotifyGive(task);
    }
    return result;
}

audio_io_result_t audio_io_playback_discard(
    audio_io_t *audio_io,
    uint64_t connection_epoch,
    const device_media_playback_start_t *playback_start)
{
    if (audio_io == NULL || connection_epoch == 0U || playback_start == NULL ||
        playback_start->stream_id == 0U ||
        !identifier_is_terminated(playback_start->output_id,
                                  sizeof(playback_start->output_id))) {
        return AUDIO_IO_RESULT_INVALID_ARGUMENT;
    }
    if (xSemaphoreTake(audio_io->lock, AUDIO_IO_INGRESS_LOCK_WAIT_TICKS) != pdTRUE) {
        return AUDIO_IO_RESULT_BUSY;
    }
    if (!audio_io->started ||
        (!audio_io->capture_requested && !audio_io->capture_active &&
         !audio_io->hardware_capturing)) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_INVALID_STATE;
    }
    if (audio_io->discarded_session_active &&
        (audio_io->discarded_connection_epoch != connection_epoch ||
         audio_io->discarded_stream_id != playback_start->stream_id)) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_BUSY;
    }
    audio_io->discarded_session_active = true;
    audio_io->discarded_connection_epoch = connection_epoch;
    audio_io->discarded_stream_id = playback_start->stream_id;
    memcpy(audio_io->discarded_output_id, playback_start->output_id,
           sizeof(audio_io->discarded_output_id));
    xSemaphoreGive(audio_io->lock);
    return AUDIO_IO_RESULT_OK;
}

audio_io_result_t audio_io_playback_push(
    audio_io_t *audio_io,
    uint64_t connection_epoch,
    const device_pcm_frame_view_t *frame)
{
    if (audio_io == NULL || connection_epoch == 0U || frame == NULL) {
        return AUDIO_IO_RESULT_INVALID_ARGUMENT;
    }
    if (frame->direction != DEVICE_PCM_DIRECTION_DOWNLINK || frame->stream_id == 0U ||
        frame->payload == NULL || frame->payload_size != DEVICE_PCM_PAYLOAD_BYTES) {
        return AUDIO_IO_RESULT_INVALID_ARGUMENT;
    }
    if (xSemaphoreTake(audio_io->lock, AUDIO_IO_INGRESS_LOCK_WAIT_TICKS) != pdTRUE) {
        return AUDIO_IO_RESULT_BUSY;
    }
    if (audio_io->discarded_session_active &&
        audio_io->discarded_connection_epoch == connection_epoch &&
        audio_io->discarded_stream_id == frame->stream_id) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_OK;
    }
    const audio_io_result_t result = queue_result(audio_playback_queue_push(
        audio_io->playback_queue,
        connection_epoch,
        frame));
    if (result == AUDIO_IO_RESULT_OK) {
        update_playback_status(audio_io);
    }
    TaskHandle_t task = audio_io->task;
    xSemaphoreGive(audio_io->lock);
    if (result == AUDIO_IO_RESULT_OK && task != NULL) {
        xTaskNotifyGive(task);
    }
    return result;
}

static audio_io_result_t cancel_active_playback(
    audio_io_t *audio_io,
    bool retain_discarded_session)
{
    copy_active_event(
        audio_io,
        AUDIO_IO_EVENT_PLAYBACK_CANCELLED,
        AUDIO_IO_RESULT_OK,
        &audio_io->cancellation_event);
    if (retain_discarded_session) {
        audio_io->discarded_session_active = true;
        audio_io->discarded_connection_epoch = audio_io->status.connection_epoch;
        audio_io->discarded_stream_id = audio_io->status.stream_id;
        memcpy(audio_io->discarded_output_id, audio_io->output_id,
               sizeof(audio_io->discarded_output_id));
    }
    const audio_hardware_result_t mute_result =
        audio_hardware_playback_mute(audio_io->hardware);
    if (mute_result != AUDIO_HARDWARE_RESULT_OK) {
        audio_io->cancellation_event.type = AUDIO_IO_EVENT_PLAYBACK_FAILED;
        audio_io->cancellation_event.result = AUDIO_IO_RESULT_HARDWARE_FAILURE;
    }
    audio_io->cancellation_pending = true;
    clear_playback_queue(audio_io);
    return mute_result == AUDIO_HARDWARE_RESULT_OK ? AUDIO_IO_RESULT_OK
                                                   : AUDIO_IO_RESULT_HARDWARE_FAILURE;
}

audio_io_result_t audio_io_playback_end(
    audio_io_t *audio_io,
    uint64_t connection_epoch,
    const device_media_playback_end_t *playback_end)
{
    if (audio_io == NULL || connection_epoch == 0U || playback_end == NULL ||
        playback_end->stream_id == 0U ||
        !identifier_is_terminated(playback_end->output_id, sizeof(playback_end->output_id)) ||
        !identifier_is_terminated(playback_end->reason, sizeof(playback_end->reason))) {
        return AUDIO_IO_RESULT_INVALID_ARGUMENT;
    }
    if (xSemaphoreTake(audio_io->lock, AUDIO_IO_INGRESS_LOCK_WAIT_TICKS) != pdTRUE) {
        return AUDIO_IO_RESULT_BUSY;
    }
    if (audio_io->discarded_session_active &&
        audio_io->discarded_connection_epoch == connection_epoch &&
        audio_io->discarded_stream_id == playback_end->stream_id &&
        strcmp(audio_io->discarded_output_id, playback_end->output_id) == 0) {
        audio_io->discarded_session_active = false;
        memset(audio_io->discarded_output_id, 0, sizeof(audio_io->discarded_output_id));
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_OK;
    }
    if (!audio_io->playback_active) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_SESSION_MISMATCH;
    }
    audio_io_result_t result;
    if (strcmp(playback_end->reason, "completed") == 0) {
        result = queue_result(audio_playback_queue_end(
            audio_io->playback_queue,
            connection_epoch,
            playback_end));
        if (result == AUDIO_IO_RESULT_OK) {
            update_playback_status(audio_io);
        }
    } else if (audio_io->status.connection_epoch == connection_epoch &&
               audio_io->status.stream_id == playback_end->stream_id &&
               strcmp(audio_io->output_id, playback_end->output_id) == 0) {
        result = cancel_active_playback(audio_io, false);
    } else {
        result = queue_result(audio_playback_queue_discard_tail(
            audio_io->playback_queue,
            connection_epoch,
            playback_end));
        if (result == AUDIO_IO_RESULT_OK) {
            update_playback_status(audio_io);
        }
    }
    TaskHandle_t task = audio_io->task;
    xSemaphoreGive(audio_io->lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    return result;
}

audio_io_result_t audio_io_playback_cancel(audio_io_t *audio_io, uint64_t connection_epoch)
{
    if (audio_io == NULL || connection_epoch == 0U) {
        return AUDIO_IO_RESULT_INVALID_ARGUMENT;
    }
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    if (!audio_io->playback_active) {
        const audio_io_result_t result =
            audio_io->discarded_session_active &&
                    audio_io->discarded_connection_epoch == connection_epoch
                ? AUDIO_IO_RESULT_OK
                : AUDIO_IO_RESULT_INVALID_STATE;
        xSemaphoreGive(audio_io->lock);
        return result;
    }
    if (audio_io->status.connection_epoch != connection_epoch) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_SESSION_MISMATCH;
    }
    const audio_io_result_t result = cancel_active_playback(audio_io, true);
    TaskHandle_t task = audio_io->task;
    xSemaphoreGive(audio_io->lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    return result;
}

void audio_io_get_status(audio_io_t *audio_io, audio_io_status_t *status)
{
    if (audio_io == NULL || status == NULL) {
        return;
    }
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    *status = audio_io->status;
    audio_playback_queue_status_t queue_status = {0};
    audio_playback_queue_get_status(audio_io->playback_queue, &queue_status);
    status->total_queued_frame_count = queue_status.total_queued_frame_count;
    status->playback_frame_high_watermark = queue_status.frame_high_watermark;
    status->playback_stream_high_watermark = queue_status.stream_high_watermark;
    xSemaphoreGive(audio_io->lock);
}

void audio_io_stop(audio_io_t *audio_io)
{
    if (audio_io == NULL) {
        return;
    }
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    if (!audio_io->started) {
        xSemaphoreGive(audio_io->lock);
        return;
    }
    audio_io->stop_requested = true;
    audio_io->cancellation_pending = false;
    audio_io->discarded_session_active = false;
    (void)audio_hardware_playback_mute(audio_io->hardware);
    TaskHandle_t task = audio_io->task;
    xSemaphoreGive(audio_io->lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    (void)xEventGroupWaitBits(
        audio_io->lifecycle_events,
        AUDIO_IO_STOPPED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY);
}

void audio_io_destroy(audio_io_t *audio_io)
{
    if (audio_io == NULL) {
        return;
    }
    audio_io_stop(audio_io);
    audio_hardware_destroy(audio_io->hardware);
    audio_playback_queue_destroy(audio_io->playback_queue);
    vEventGroupDelete(audio_io->lifecycle_events);
    vSemaphoreDelete(audio_io->lock);
    memset(audio_io, 0, sizeof(*audio_io));
    free(audio_io);
}
