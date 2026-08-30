#include "audio_io.h"

#include <stdlib.h>
#include <string.h>

#include "audio_hardware.h"
#include "audio_pcm_conversion.h"
#include "audio_playback_ring.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define AUDIO_IO_TASK_STACK_BYTES 8192U
#define AUDIO_IO_TASK_PRIORITY 7U
#define AUDIO_IO_PLAYBACK_WRITE_TIMEOUT_MS 100U
#define AUDIO_IO_STOPPED_BIT BIT0

struct audio_io {
    SemaphoreHandle_t lock;
    EventGroupHandle_t lifecycle_events;
    TaskHandle_t task;
    audio_hardware_t *hardware;
    audio_playback_ring_t *playback_ring;
    audio_io_event_callback_t event_callback;
    void *event_context;
    uint8_t playback_gain_percent;
    bool started;
    bool stop_requested;
    bool playback_active;
    bool hardware_playing;
    uint64_t playback_generation;
    char output_id[DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES + 1U];
    bool cancellation_pending;
    audio_io_event_t cancellation_event;
    bool discarded_session_active;
    uint64_t discarded_connection_epoch;
    uint32_t discarded_stream_id;
    char discarded_output_id[DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES + 1U];
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
    };
    memcpy(event->output_id, audio_io->output_id, sizeof(event->output_id));
}

static audio_io_result_t ring_result(audio_playback_ring_result_t result)
{
    switch (result) {
    case AUDIO_PLAYBACK_RING_RESULT_OK:
        return AUDIO_IO_RESULT_OK;
    case AUDIO_PLAYBACK_RING_RESULT_INVALID_ARGUMENT:
        return AUDIO_IO_RESULT_INVALID_ARGUMENT;
    case AUDIO_PLAYBACK_RING_RESULT_NO_MEMORY:
        return AUDIO_IO_RESULT_NO_MEMORY;
    case AUDIO_PLAYBACK_RING_RESULT_INVALID_STATE:
    case AUDIO_PLAYBACK_RING_RESULT_EMPTY:
        return AUDIO_IO_RESULT_INVALID_STATE;
    case AUDIO_PLAYBACK_RING_RESULT_STREAM_MISMATCH:
        return AUDIO_IO_RESULT_SESSION_MISMATCH;
    case AUDIO_PLAYBACK_RING_RESULT_SEQUENCE_MISMATCH:
        return AUDIO_IO_RESULT_SEQUENCE_MISMATCH;
    case AUDIO_PLAYBACK_RING_RESULT_OVERFLOW:
        return AUDIO_IO_RESULT_BUFFER_OVERFLOW;
    case AUDIO_PLAYBACK_RING_RESULT_SAMPLE_COUNT_EXCEEDED:
        return AUDIO_IO_RESULT_SAMPLE_COUNT_EXCEEDED;
    case AUDIO_PLAYBACK_RING_RESULT_INCOMPLETE:
        return AUDIO_IO_RESULT_INCOMPLETE;
    default:
        return AUDIO_IO_RESULT_INVALID_STATE;
    }
}

static void clear_active_playback(audio_io_t *audio_io)
{
    audio_playback_ring_clear(audio_io->playback_ring);
    audio_io->playback_active = false;
    ++audio_io->playback_generation;
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
}

static void update_ring_status(audio_io_t *audio_io)
{
    audio_playback_ring_status_t ring_status = {0};
    audio_playback_ring_get_status(audio_io->playback_ring, &ring_status);
    audio_io->status.connection_epoch = ring_status.connection_epoch;
    audio_io->status.stream_id = ring_status.stream_id;
    audio_io->status.sample_count = ring_status.sample_count;
    audio_io->status.received_frame_count = ring_status.received_frame_count;
    audio_io->status.consumed_sample_count = ring_status.consumed_sample_count;
    audio_io->status.queued_frame_count = ring_status.queued_frame_count;
}

static void record_event_count(audio_io_t *audio_io, audio_io_event_type_t type)
{
    if (type == AUDIO_IO_EVENT_PLAYBACK_COMPLETED) {
        ++audio_io->status.completed_playback_count;
    } else if (type == AUDIO_IO_EVENT_PLAYBACK_CANCELLED) {
        ++audio_io->status.cancelled_playback_count;
    } else if (type == AUDIO_IO_EVENT_PLAYBACK_FAILED) {
        ++audio_io->status.failed_playback_count;
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
        clear_active_playback(audio_io);
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

static void audio_io_task(void *context)
{
    audio_io_t *audio_io = context;
    int16_t pcm_samples[AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME];
    int32_t i2s_words[AUDIO_PLAYBACK_RING_SAMPLES_PER_FRAME * AUDIO_PCM_I2S_WORDS_PER_FRAME];
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
        if (!audio_io->playback_active) {
            xSemaphoreGive(audio_io->lock);
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        const uint64_t generation = audio_io->playback_generation;
        if (!audio_io->hardware_playing) {
            if (!audio_playback_ring_ready(audio_io->playback_ring)) {
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
            xSemaphoreTake(audio_io->lock, portMAX_DELAY);
            if (!audio_io->stop_requested && audio_io->playback_active &&
                audio_io->playback_generation == generation) {
                audio_io->hardware_playing = true;
                if (audio_hardware_playback_unmute(audio_io->hardware) ==
                    AUDIO_HARDWARE_RESULT_OK) {
                    audio_io->status.playback_state = AUDIO_IO_PLAYBACK_STATE_PLAYING;
                    copy_active_event(
                        audio_io,
                        AUDIO_IO_EVENT_PLAYBACK_STARTED,
                        AUDIO_IO_RESULT_OK,
                        &started_event);
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
            emit_event(audio_io, &started_event);
            continue;
        }
        if (audio_playback_ring_drained(audio_io->playback_ring)) {
            xSemaphoreGive(audio_io->lock);
            playback_finish(
                audio_io,
                generation,
                AUDIO_IO_EVENT_PLAYBACK_COMPLETED,
                AUDIO_IO_RESULT_OK,
                true);
            continue;
        }
        size_t pcm_sample_count = 0U;
        const audio_playback_ring_result_t pop_result =
            audio_playback_ring_pop(audio_io->playback_ring, pcm_samples, &pcm_sample_count);
        update_ring_status(audio_io);
        xSemaphoreGive(audio_io->lock);
        if (pop_result == AUDIO_PLAYBACK_RING_RESULT_EMPTY) {
            (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        if (pop_result != AUDIO_PLAYBACK_RING_RESULT_OK) {
            playback_finish(
                audio_io,
                generation,
                AUDIO_IO_EVENT_PLAYBACK_FAILED,
                ring_result(pop_result),
                true);
            continue;
        }
        size_t i2s_word_count = 0U;
        if (audio_pcm_playback_convert(
                pcm_samples,
                pcm_sample_count,
                audio_io->playback_gain_percent,
                i2s_words,
                sizeof(i2s_words) / sizeof(i2s_words[0]),
                &i2s_word_count) != AUDIO_PCM_CONVERSION_RESULT_OK ||
            audio_hardware_playback_write(
                audio_io->hardware,
                i2s_words,
                i2s_word_count,
                AUDIO_IO_PLAYBACK_WRITE_TIMEOUT_MS) != AUDIO_HARDWARE_RESULT_OK) {
            playback_finish(
                audio_io,
                generation,
                AUDIO_IO_EVENT_PLAYBACK_FAILED,
                AUDIO_IO_RESULT_HARDWARE_FAILURE,
                true);
        }
    }

    (void)audio_hardware_playback_mute(audio_io->hardware);
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    const bool hardware_started = audio_io->hardware_playing;
    xSemaphoreGive(audio_io->lock);
    if (hardware_started) {
        (void)audio_hardware_playback_stop(audio_io->hardware);
    }
    xSemaphoreTake(audio_io->lock, portMAX_DELAY);
    audio_io->hardware_playing = false;
    clear_active_playback(audio_io);
    audio_io->started = false;
    audio_io->stop_requested = false;
    audio_io->task = NULL;
    audio_io->status.playback_state = AUDIO_IO_PLAYBACK_STATE_STOPPED;
    xSemaphoreGive(audio_io->lock);
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
    const audio_playback_ring_result_t ring_create_result =
        audio_playback_ring_create(&created->playback_ring);
    if (ring_create_result != AUDIO_PLAYBACK_RING_RESULT_OK) {
        vEventGroupDelete(created->lifecycle_events);
        vSemaphoreDelete(created->lock);
        free(created);
        return ring_result(ring_create_result);
    }
    if (audio_hardware_create(&created->hardware) != AUDIO_HARDWARE_RESULT_OK) {
        audio_hardware_destroy(created->hardware);
        audio_playback_ring_destroy(created->playback_ring);
        vEventGroupDelete(created->lifecycle_events);
        vSemaphoreDelete(created->lock);
        free(created);
        return AUDIO_IO_RESULT_HARDWARE_FAILURE;
    }
    created->event_callback = config->event_callback;
    created->event_context = config->event_context;
    created->playback_gain_percent = config->playback_gain_percent;
    created->status.playback_state = AUDIO_IO_PLAYBACK_STATE_STOPPED;
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
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_NO_MEMORY;
    }
    xSemaphoreGive(audio_io->lock);
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
    if (xSemaphoreTake(audio_io->lock, 0U) != pdTRUE) {
        return AUDIO_IO_RESULT_BUSY;
    }
    if (!audio_io->started) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_INVALID_STATE;
    }
    if (audio_io->playback_active) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_INVALID_STATE;
    }
    if (audio_io->cancellation_pending || audio_io->hardware_playing) {
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
    const audio_io_result_t result = ring_result(audio_playback_ring_begin(
        audio_io->playback_ring,
        connection_epoch,
        playback_start->stream_id,
        playback_start->sample_count));
    if (result == AUDIO_IO_RESULT_OK) {
        audio_io->playback_active = true;
        ++audio_io->playback_generation;
        memcpy(audio_io->output_id, playback_start->output_id, sizeof(audio_io->output_id));
        audio_io->status.playback_state = AUDIO_IO_PLAYBACK_STATE_BUFFERING;
        memcpy(audio_io->status.output_id, playback_start->output_id,
               sizeof(audio_io->status.output_id));
        update_ring_status(audio_io);
    }
    TaskHandle_t task = audio_io->task;
    xSemaphoreGive(audio_io->lock);
    if (result == AUDIO_IO_RESULT_OK && task != NULL) {
        xTaskNotifyGive(task);
    }
    return result;
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
    if (xSemaphoreTake(audio_io->lock, 0U) != pdTRUE) {
        return AUDIO_IO_RESULT_BUSY;
    }
    if (audio_io->discarded_session_active &&
        audio_io->discarded_connection_epoch == connection_epoch &&
        audio_io->discarded_stream_id == frame->stream_id) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_OK;
    }
    const audio_io_result_t result =
        ring_result(audio_playback_ring_push(audio_io->playback_ring, connection_epoch, frame));
    if (result == AUDIO_IO_RESULT_OK) {
        update_ring_status(audio_io);
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
    clear_active_playback(audio_io);
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
    if (xSemaphoreTake(audio_io->lock, 0U) != pdTRUE) {
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
    if (!audio_io->playback_active || audio_io->status.connection_epoch != connection_epoch ||
        audio_io->status.stream_id != playback_end->stream_id ||
        strcmp(audio_io->output_id, playback_end->output_id) != 0) {
        xSemaphoreGive(audio_io->lock);
        return AUDIO_IO_RESULT_SESSION_MISMATCH;
    }
    audio_io_result_t result;
    if (strcmp(playback_end->reason, "completed") == 0) {
        result = ring_result(audio_playback_ring_end(
            audio_io->playback_ring,
            connection_epoch,
            playback_end->stream_id));
        if (result == AUDIO_IO_RESULT_OK) {
            update_ring_status(audio_io);
        }
    } else {
        result = cancel_active_playback(audio_io, false);
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
    if (xSemaphoreTake(audio_io->lock, 0U) != pdTRUE) {
        return AUDIO_IO_RESULT_BUSY;
    }
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
    audio_playback_ring_destroy(audio_io->playback_ring);
    vEventGroupDelete(audio_io->lifecycle_events);
    vSemaphoreDelete(audio_io->lock);
    memset(audio_io, 0, sizeof(*audio_io));
    free(audio_io);
}
