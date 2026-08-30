#include <stdbool.h>
#include <string.h>

#include "audio_io.h"
#include "device_identity.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "device_connection.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "network_connectivity.h"
#include "nvs_flash.h"
#include "terminal_ui.h"
#include "terminal_interaction.h"
#include "voice_input.h"

#define TERMINAL_EVENT_QUEUE_LENGTH 32U
#define TERMINAL_LIFECYCLE_TASK_STACK_BYTES 4096U
#define TERMINAL_LIFECYCLE_TASK_PRIORITY 5U
#define TERMINAL_RESOURCE_MONITOR_TASK_STACK_BYTES 4096U
#define TERMINAL_RESOURCE_MONITOR_TASK_PRIORITY 1U
#define TERMINAL_RESOURCE_MONITOR_INITIAL_DELAY_MS 10000U
#define TERMINAL_RESOURCE_MONITOR_INTERVAL_MS 30000U
#define TERMINAL_RESOURCE_MONITOR_MAX_TASKS 32U

static const char *const TAG = "terminal_lifecycle";
typedef enum {
    TERMINAL_EVENT_NETWORK = 0,
    TERMINAL_EVENT_DEVICE_CONNECTION,
    TERMINAL_EVENT_DEVICE_MEDIA_STATE,
    TERMINAL_EVENT_AUDIO_IO,
    TERMINAL_EVENT_INTERACTION,
    TERMINAL_EVENT_VOICE_INPUT,
} terminal_event_type_t;

typedef struct {
    uint64_t connection_epoch;
    device_media_interaction_state_t state;
} terminal_media_state_event_t;

typedef struct {
    audio_io_event_type_t type;
    audio_io_result_t result;
    uint64_t connection_epoch;
    uint32_t stream_id;
    uint64_t capture_session_id;
    uint64_t capture_sample_count;
    uint32_t playback_rebuffer_count;
} terminal_audio_io_event_t;

typedef struct {
    terminal_event_type_t type;
    union {
        network_connectivity_event_t network;
        device_connection_event_t device_connection;
        terminal_media_state_event_t media_state;
        terminal_audio_io_event_t audio_io;
        terminal_interaction_event_t interaction;
        voice_input_event_t voice_input;
    } value;
} terminal_event_t;

static QueueHandle_t terminal_event_queue;
static audio_io_t *terminal_audio_io;
static network_connectivity_t *terminal_network_connectivity;
static device_connection_t *terminal_device_connection;
static device_identity_t *terminal_device_identity;
static terminal_interaction_t *terminal_interaction;
static terminal_ui_t *terminal_user_interface;
static voice_input_t *terminal_voice_input;
static uint32_t terminal_event_drop_count;
static uint32_t terminal_device_network_update_error_count;
static uint32_t terminal_ui_update_error_count;
static uint32_t terminal_audio_ingress_error_count;
static uint32_t terminal_audio_ingress_last_result;
static uint32_t terminal_audio_cancel_error_count;
static uint32_t terminal_voice_input_error_count;
static uint32_t terminal_interaction_error_count;
static bool terminal_device_paired;
static bool terminal_device_online;
static uint64_t terminal_online_connection_epoch;
static uint64_t terminal_capture_session_id;
static device_pairing_state_t terminal_pairing_state = DEVICE_PAIRING_STATE_READY;

static const char *const terminal_observed_task_names[] = {
    "terminal_lifecycle",
    "device_connection",
    "audio_io",
    "voice_input",
    "terminal_ui",
    "terminal_button",
};

static void terminal_device_connection_network_changed(
    bool has_ip,
    uint64_t ip_epoch);

static terminal_ui_state_t terminal_connection_ui_state(void)
{
    if (terminal_device_paired || terminal_pairing_state == DEVICE_PAIRING_STATE_COMPLETE) {
        return TERMINAL_UI_STATE_CONNECTING;
    }
    if (terminal_pairing_state >= DEVICE_PAIRING_STATE_WAITING_FOR_APPROVAL &&
        terminal_pairing_state < DEVICE_PAIRING_STATE_COMPLETE) {
        return TERMINAL_UI_STATE_PAIRING;
    }
    return TERMINAL_UI_STATE_WAITING_FOR_PAIRING;
}

static void terminal_ui_project(
    terminal_ui_state_t state,
    terminal_ui_error_t error)
{
    if (terminal_user_interface == NULL) {
        return;
    }
    terminal_ui_projection_t projection = {
        .state = state,
        .error = error,
    };
    if (!terminal_device_paired &&
        state != TERMINAL_UI_STATE_STARTING &&
        state != TERMINAL_UI_STATE_ERROR) {
        projection.pairing_code_visible =
            terminal_device_connection != NULL &&
            device_connection_copy_pairing_code(
                terminal_device_connection,
                projection.pairing_code) == DEVICE_CONNECTION_RESULT_OK;
    }
    if (terminal_ui_update(terminal_user_interface, &projection) !=
        TERMINAL_UI_RESULT_OK) {
        (void)__atomic_fetch_add(
            &terminal_ui_update_error_count,
            1U,
            __ATOMIC_RELAXED);
    }
}

static terminal_ui_error_t terminal_connection_error(
    const device_connection_event_t *event)
{
    if (event->selection_result == HOST_SELECTION_MULTIPLE_PAIRING_HOSTS) {
        return TERMINAL_UI_ERROR_MULTIPLE_HOSTS;
    }
    if (event->failure == DEVICE_CONNECTION_FAILURE_CERTIFICATE ||
        event->block_reason == DEVICE_CONNECTION_BLOCK_CERTIFICATE) {
        return TERMINAL_UI_ERROR_CERTIFICATE;
    }
    if (event->failure == DEVICE_CONNECTION_FAILURE_IDENTITY ||
        event->block_reason == DEVICE_CONNECTION_BLOCK_IDENTITY) {
        return TERMINAL_UI_ERROR_IDENTITY;
    }
    if (event->failure == DEVICE_CONNECTION_FAILURE_PROTOCOL ||
        event->block_reason == DEVICE_CONNECTION_BLOCK_PROTOCOL) {
        return TERMINAL_UI_ERROR_PROTOCOL;
    }
    return TERMINAL_UI_ERROR_NONE;
}

static void terminal_device_connection_ui_project(
    const device_connection_event_t *event)
{
    terminal_device_paired = event->paired;
    terminal_pairing_state = event->pairing_state;
    if (event->pairing_state == DEVICE_PAIRING_STATE_COMPLETE) {
        terminal_ui_project(TERMINAL_UI_STATE_CONNECTING, TERMINAL_UI_ERROR_NONE);
        return;
    }
    if (event->type == DEVICE_CONNECTION_EVENT_FAILURE) {
        const terminal_ui_error_t error = terminal_connection_error(event);
        if (error != TERMINAL_UI_ERROR_NONE) {
            terminal_ui_project(TERMINAL_UI_STATE_ERROR, error);
            return;
        }
    }
    if (event->state == DEVICE_CONNECTION_STATE_ONLINE) {
        terminal_ui_project(TERMINAL_UI_STATE_READY, TERMINAL_UI_ERROR_NONE);
        return;
    }
    if (event->state == DEVICE_CONNECTION_STATE_BLOCKED) {
        terminal_ui_project(
            TERMINAL_UI_STATE_ERROR,
            terminal_connection_error(event) == TERMINAL_UI_ERROR_NONE
                ? TERMINAL_UI_ERROR_PROTOCOL
                : terminal_connection_error(event));
        return;
    }
    terminal_ui_project(terminal_connection_ui_state(), TERMINAL_UI_ERROR_NONE);
}

static void terminal_network_ui_project(
    const network_connectivity_event_t *event)
{
    if (event->state == NETWORK_CONNECTIVITY_STATE_ERROR ||
        event->state == NETWORK_CONNECTIVITY_STATE_UNCONFIGURED) {
        terminal_ui_project(TERMINAL_UI_STATE_ERROR, TERMINAL_UI_ERROR_NETWORK);
        return;
    }
    if (event->state != NETWORK_CONNECTIVITY_STATE_READY) {
        terminal_ui_project(
            terminal_device_paired ? TERMINAL_UI_STATE_CONNECTING
                                   : terminal_connection_ui_state(),
            TERMINAL_UI_ERROR_NONE);
    }
}

static const char *network_state_name(network_connectivity_state_t state)
{
    switch (state) {
        case NETWORK_CONNECTIVITY_STATE_STOPPED:
            return "stopped";
        case NETWORK_CONNECTIVITY_STATE_UNCONFIGURED:
            return "unconfigured";
        case NETWORK_CONNECTIVITY_STATE_CONNECTING:
            return "connecting";
        case NETWORK_CONNECTIVITY_STATE_READY:
            return "ready";
        case NETWORK_CONNECTIVITY_STATE_BACKOFF:
            return "backoff";
        case NETWORK_CONNECTIVITY_STATE_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

static void terminal_network_event(
    const network_connectivity_event_t *event,
    void *context)
{
    QueueHandle_t event_queue = context;
    if (event->type == NETWORK_CONNECTIVITY_EVENT_IP_ACQUIRED) {
        terminal_device_connection_network_changed(true, event->ip_epoch);
    } else if (event->type == NETWORK_CONNECTIVITY_EVENT_IP_LOST) {
        terminal_device_connection_network_changed(false, event->ip_epoch);
    }
    const terminal_event_t terminal_event = {
        .type = TERMINAL_EVENT_NETWORK,
        .value.network = *event,
    };
    if (event_queue == NULL || xQueueSend(event_queue, &terminal_event, 0U) != pdTRUE) {
        (void)__atomic_fetch_add(&terminal_event_drop_count, 1U, __ATOMIC_RELAXED);
    }
}

static void terminal_device_connection_event(
    const device_connection_event_t *event,
    void *context)
{
    QueueHandle_t event_queue = context;
    const terminal_event_t terminal_event = {
        .type = TERMINAL_EVENT_DEVICE_CONNECTION,
        .value.device_connection = *event,
    };
    if (event_queue == NULL || xQueueSend(event_queue, &terminal_event, 0U) != pdTRUE) {
        (void)__atomic_fetch_add(&terminal_event_drop_count, 1U, __ATOMIC_RELAXED);
    }
}

static void terminal_audio_io_event(
    const audio_io_event_t *event,
    void *context)
{
    QueueHandle_t event_queue = context;
    const terminal_event_t terminal_event = {
        .type = TERMINAL_EVENT_AUDIO_IO,
        .value.audio_io = {
            .type = event->type,
            .result = event->result,
            .connection_epoch = event->connection_epoch,
            .stream_id = event->stream_id,
            .capture_session_id = event->capture_session_id,
            .capture_sample_count = event->capture_sample_count,
            .playback_rebuffer_count = event->playback_rebuffer_count,
        },
    };
    if (event_queue == NULL || xQueueSend(event_queue, &terminal_event, 0U) != pdTRUE) {
        (void)__atomic_fetch_add(&terminal_event_drop_count, 1U, __ATOMIC_RELAXED);
    }
}

static bool terminal_audio_capture_frame(
    uint64_t capture_session_id,
    const int16_t samples[AUDIO_IO_CAPTURE_SAMPLES_PER_FRAME],
    size_t sample_count,
    void *context)
{
    (void)context;
    return terminal_voice_input != NULL &&
           voice_input_capture_frame(
               terminal_voice_input,
               capture_session_id,
               samples,
               sample_count);
}

static void terminal_interaction_event(
    const terminal_interaction_event_t *event,
    void *context)
{
    QueueHandle_t event_queue = context;
    const terminal_event_t terminal_event = {
        .type = TERMINAL_EVENT_INTERACTION,
        .value.interaction = *event,
    };
    if (event_queue == NULL || xQueueSend(event_queue, &terminal_event, 0U) != pdTRUE) {
        (void)__atomic_fetch_add(&terminal_event_drop_count, 1U, __ATOMIC_RELAXED);
    }
}

static void terminal_voice_input_event(
    const voice_input_event_t *event,
    void *context)
{
    QueueHandle_t event_queue = context;
    const terminal_event_t terminal_event = {
        .type = TERMINAL_EVENT_VOICE_INPUT,
        .value.voice_input = *event,
    };
    if (event_queue == NULL || xQueueSend(event_queue, &terminal_event, 0U) != pdTRUE) {
        (void)__atomic_fetch_add(&terminal_event_drop_count, 1U, __ATOMIC_RELAXED);
    }
}

static bool terminal_device_media_event(
    const device_connection_media_event_t *event,
    void *context)
{
    if (event->type == DEVICE_CONNECTION_MEDIA_EVENT_STATE_CHANGED) {
        if (event->value.state_changed.state == DEVICE_MEDIA_INTERACTION_STATE_IDLE &&
            event->value.state_changed.has_client_input_id &&
            terminal_voice_input != NULL &&
            voice_input_reject(
                terminal_voice_input,
                event->connection_epoch,
                event->value.state_changed.client_input_id) != VOICE_INPUT_RESULT_OK) {
            (void)__atomic_fetch_add(
                &terminal_voice_input_error_count,
                1U,
                __ATOMIC_RELAXED);
            return false;
        }
        QueueHandle_t event_queue = context;
        const terminal_event_t terminal_event = {
            .type = TERMINAL_EVENT_DEVICE_MEDIA_STATE,
            .value.media_state = {
                .connection_epoch = event->connection_epoch,
                .state = event->value.state_changed.state,
            },
        };
        if (event_queue == NULL || xQueueSend(event_queue, &terminal_event, 0U) != pdTRUE) {
            (void)__atomic_fetch_add(&terminal_event_drop_count, 1U, __ATOMIC_RELAXED);
        }
        return true;
    }
    if (event->type == DEVICE_CONNECTION_MEDIA_EVENT_REMOTE_ERROR) {
        ESP_LOGI(
            TAG,
            "event=device_remote_error code=%s recoverable=%u connection_epoch=%llu",
            event->value.remote_error.code,
            event->value.remote_error.recoverable ? 1U : 0U,
            (unsigned long long)event->connection_epoch);
        return true;
    }
    if (event->type == DEVICE_CONNECTION_MEDIA_EVENT_INPUT_ACCEPTED) {
        if (terminal_voice_input == NULL ||
            voice_input_accept(
                terminal_voice_input,
                event->connection_epoch,
                &event->value.input_accepted) != VOICE_INPUT_RESULT_OK) {
            (void)__atomic_fetch_add(
                &terminal_voice_input_error_count,
                1U,
                __ATOMIC_RELAXED);
            return false;
        }
        QueueHandle_t event_queue = context;
        const terminal_event_t terminal_event = {
            .type = TERMINAL_EVENT_DEVICE_MEDIA_STATE,
            .value.media_state = {
                .connection_epoch = event->connection_epoch,
                .state = DEVICE_MEDIA_INTERACTION_STATE_PROCESSING,
            },
        };
        if (event_queue == NULL || xQueueSend(event_queue, &terminal_event, 0U) != pdTRUE) {
            (void)__atomic_fetch_add(&terminal_event_drop_count, 1U, __ATOMIC_RELAXED);
        }
        return true;
    }
    audio_io_result_t result = AUDIO_IO_RESULT_OK;
    if (terminal_audio_io == NULL) {
        result = AUDIO_IO_RESULT_INVALID_STATE;
    } else if (event->type == DEVICE_CONNECTION_MEDIA_EVENT_PLAYBACK_START) {
        result = audio_io_playback_begin(
            terminal_audio_io,
            event->connection_epoch,
            &event->value.playback_start);
    } else if (event->type == DEVICE_CONNECTION_MEDIA_EVENT_PCM_FRAME) {
        result = audio_io_playback_push(
            terminal_audio_io,
            event->connection_epoch,
            &event->value.pcm_frame);
    } else if (event->type == DEVICE_CONNECTION_MEDIA_EVENT_PLAYBACK_END) {
        result = audio_io_playback_end(
            terminal_audio_io,
            event->connection_epoch,
            &event->value.playback_end);
    }
    if (result != AUDIO_IO_RESULT_OK) {
        __atomic_store_n(
            &terminal_audio_ingress_last_result,
            (uint32_t)result,
            __ATOMIC_RELAXED);
        (void)__atomic_fetch_add(&terminal_audio_ingress_error_count, 1U, __ATOMIC_RELAXED);
        return false;
    }
    return true;
}

static const char *device_connection_state_name(device_connection_state_t state)
{
    switch (state) {
        case DEVICE_CONNECTION_STATE_WIFI_DOWN:
            return "wifi_down";
        case DEVICE_CONNECTION_STATE_DISCOVERING:
            return "discovering";
        case DEVICE_CONNECTION_STATE_CONNECTING:
            return "connecting";
        case DEVICE_CONNECTION_STATE_AUTHENTICATING:
            return "authenticating";
        case DEVICE_CONNECTION_STATE_ONLINE:
            return "online";
        case DEVICE_CONNECTION_STATE_BACKOFF:
            return "backoff";
        case DEVICE_CONNECTION_STATE_BLOCKED:
            return "blocked";
        default:
            return "unknown";
    }
}

static void terminal_device_connection_event_process(
    const device_connection_event_t *event)
{
    if (event->state == DEVICE_CONNECTION_STATE_ONLINE) {
        const bool new_online_epoch = !terminal_device_online ||
                                      terminal_online_connection_epoch !=
                                          event->connection_epoch;
        terminal_device_online = true;
        terminal_online_connection_epoch = event->connection_epoch;
        if (new_online_epoch && terminal_voice_input != NULL &&
            voice_input_connection_changed(
                terminal_voice_input,
                true,
                event->connection_epoch) != VOICE_INPUT_RESULT_OK) {
            (void)__atomic_fetch_add(
                &terminal_voice_input_error_count,
                1U,
                __ATOMIC_RELAXED);
        }
    } else if (terminal_device_online &&
               event->connection_epoch == terminal_online_connection_epoch) {
        terminal_device_online = false;
        if (terminal_voice_input != NULL &&
            voice_input_connection_changed(
                terminal_voice_input,
                false,
                event->connection_epoch) != VOICE_INPUT_RESULT_OK) {
            (void)__atomic_fetch_add(
                &terminal_voice_input_error_count,
                1U,
                __ATOMIC_RELAXED);
        }
        if (terminal_audio_io != NULL) {
            const audio_io_result_t cancel_result = audio_io_playback_cancel(
                terminal_audio_io,
                event->connection_epoch);
            if (cancel_result != AUDIO_IO_RESULT_OK &&
                cancel_result != AUDIO_IO_RESULT_INVALID_STATE) {
                (void)__atomic_fetch_add(
                    &terminal_audio_cancel_error_count,
                    1U,
                    __ATOMIC_RELAXED);
            }
        }
    }
    terminal_device_connection_ui_project(event);
    if (event->type == DEVICE_CONNECTION_EVENT_HOST_SELECTED) {
        ESP_LOGI(
            TAG,
            "event=device_host_selected state=%s host_suffix=%s connection_epoch=%llu",
            device_connection_state_name(event->state),
            event->installation_id_suffix,
            (unsigned long long)event->connection_epoch);
    } else if (event->type == DEVICE_CONNECTION_EVENT_CONTROL_RECEIVED) {
        ESP_LOGI(
            TAG,
            "event=device_control_received state=%s type=%s connection_epoch=%llu",
            device_connection_state_name(event->state),
            event->message_type,
            (unsigned long long)event->connection_epoch);
    } else if (event->type == DEVICE_CONNECTION_EVENT_FAILURE) {
        ESP_LOGW(
            TAG,
            "event=device_connection_failure state=%s failure=%u selection=%u transport=%u "
            "block=%u reconnect_delay_ms=%lu connection_epoch=%llu",
            device_connection_state_name(event->state),
            (unsigned int)event->failure,
            (unsigned int)event->selection_result,
            (unsigned int)event->transport_result,
            (unsigned int)event->block_reason,
            (unsigned long)event->reconnect_delay_ms,
            (unsigned long long)event->connection_epoch);
    } else if (event->type == DEVICE_CONNECTION_EVENT_PAIRING_STATE_CHANGED) {
        ESP_LOGI(
            TAG,
            "event=device_pairing_state_changed pairing_state=%u connection_epoch=%llu",
            (unsigned int)event->pairing_state,
            (unsigned long long)event->connection_epoch);
    } else {
        ESP_LOGI(
            TAG,
            "event=device_connection_state_changed state=%s block=%u connection_epoch=%llu",
            device_connection_state_name(event->state),
            (unsigned int)event->block_reason,
            (unsigned long long)event->connection_epoch);
    }
}

static void terminal_media_state_event_process(
    const terminal_media_state_event_t *event)
{
    if (!terminal_device_online ||
        event->connection_epoch != terminal_online_connection_epoch) {
        return;
    }
    terminal_ui_state_t ui_state;
    switch (event->state) {
        case DEVICE_MEDIA_INTERACTION_STATE_IDLE:
            ui_state = TERMINAL_UI_STATE_READY;
            break;
        case DEVICE_MEDIA_INTERACTION_STATE_LISTENING:
            ui_state = TERMINAL_UI_STATE_LISTENING;
            break;
        case DEVICE_MEDIA_INTERACTION_STATE_RECOGNIZING:
        case DEVICE_MEDIA_INTERACTION_STATE_PROCESSING:
            ui_state = TERMINAL_UI_STATE_THINKING;
            break;
        case DEVICE_MEDIA_INTERACTION_STATE_SPEAKING:
            ui_state = TERMINAL_UI_STATE_SPEAKING;
            break;
        case DEVICE_MEDIA_INTERACTION_STATE_NEEDS_ATTENTION:
        case DEVICE_MEDIA_INTERACTION_STATE_ERROR:
        case DEVICE_MEDIA_INTERACTION_STATE_UNAVAILABLE:
            ui_state = TERMINAL_UI_STATE_CHECK_DESKTOP;
            break;
        default:
            return;
    }
    terminal_ui_project(ui_state, TERMINAL_UI_ERROR_NONE);
    ESP_LOGI(
        TAG,
        "event=device_media_state_changed state=%u connection_epoch=%llu",
        (unsigned int)event->state,
        (unsigned long long)event->connection_epoch);
}

static void terminal_audio_io_event_process(
    const terminal_audio_io_event_t *event)
{
    if (event->type <= AUDIO_IO_EVENT_PLAYBACK_FAILED) {
        if (event->type == AUDIO_IO_EVENT_PLAYBACK_STARTED && terminal_device_online &&
            event->connection_epoch == terminal_online_connection_epoch) {
            terminal_ui_project(TERMINAL_UI_STATE_SPEAKING, TERMINAL_UI_ERROR_NONE);
        } else if ((event->type == AUDIO_IO_EVENT_PLAYBACK_COMPLETED ||
                    event->type == AUDIO_IO_EVENT_PLAYBACK_CANCELLED) &&
                   terminal_device_online &&
                   event->connection_epoch == terminal_online_connection_epoch) {
            terminal_ui_project(TERMINAL_UI_STATE_READY, TERMINAL_UI_ERROR_NONE);
        } else if (event->type == AUDIO_IO_EVENT_PLAYBACK_FAILED) {
            terminal_ui_project(TERMINAL_UI_STATE_ERROR, TERMINAL_UI_ERROR_AUDIO);
        }
        ESP_LOGI(
            TAG,
            "event=audio_playback type=%u result=%u stream_id=%lu rebuffer_count=%lu "
            "connection_epoch=%llu",
            (unsigned int)event->type,
            (unsigned int)event->result,
            (unsigned long)event->stream_id,
            (unsigned long)event->playback_rebuffer_count,
            (unsigned long long)event->connection_epoch);
        return;
    }

    if (event->type == AUDIO_IO_EVENT_CAPTURE_STARTED &&
        event->capture_session_id == terminal_capture_session_id) {
        terminal_ui_project(TERMINAL_UI_STATE_LISTENING, TERMINAL_UI_ERROR_NONE);
    } else if (event->type == AUDIO_IO_EVENT_CAPTURE_COMPLETED &&
               event->capture_session_id == terminal_capture_session_id) {
        const uint64_t frame_count =
            event->capture_sample_count / AUDIO_IO_CAPTURE_SAMPLES_PER_FRAME;
        const voice_input_result_t seal_result =
            event->capture_sample_count != 0U &&
                    event->capture_sample_count % AUDIO_IO_CAPTURE_SAMPLES_PER_FRAME == 0U &&
                    terminal_voice_input != NULL
                ? voice_input_seal(
                      terminal_voice_input,
                      event->capture_session_id,
                      frame_count)
                : VOICE_INPUT_RESULT_INVALID_ARGUMENT;
        terminal_capture_session_id = 0U;
        if (seal_result == VOICE_INPUT_RESULT_OK) {
            terminal_ui_project(
                terminal_device_online ? TERMINAL_UI_STATE_THINKING
                                       : TERMINAL_UI_STATE_CONNECTING,
                TERMINAL_UI_ERROR_NONE);
        } else {
            (void)__atomic_fetch_add(
                &terminal_voice_input_error_count,
                1U,
                __ATOMIC_RELAXED);
            terminal_ui_project(TERMINAL_UI_STATE_ERROR, TERMINAL_UI_ERROR_AUDIO);
        }
    } else if ((event->type == AUDIO_IO_EVENT_CAPTURE_CANCELLED ||
                event->type == AUDIO_IO_EVENT_CAPTURE_FAILED) &&
               event->capture_session_id == terminal_capture_session_id) {
        if (terminal_voice_input != NULL) {
            (void)voice_input_cancel(
                terminal_voice_input,
                event->capture_session_id);
        }
        terminal_capture_session_id = 0U;
        terminal_ui_project(
            event->type == AUDIO_IO_EVENT_CAPTURE_FAILED
                ? TERMINAL_UI_STATE_ERROR
                : (terminal_device_online ? TERMINAL_UI_STATE_READY
                                          : TERMINAL_UI_STATE_CONNECTING),
            event->type == AUDIO_IO_EVENT_CAPTURE_FAILED
                ? TERMINAL_UI_ERROR_AUDIO
                : TERMINAL_UI_ERROR_NONE);
    }
    ESP_LOGI(
        TAG,
        "event=audio_capture type=%u result=%u frames=%llu samples=%llu",
        (unsigned int)event->type,
        (unsigned int)event->result,
        (unsigned long long)(
            event->capture_sample_count / AUDIO_IO_CAPTURE_SAMPLES_PER_FRAME),
        (unsigned long long)event->capture_sample_count);
}

static void terminal_ptt_pressed_process(void)
{
    if (!terminal_device_online || terminal_voice_input == NULL ||
        terminal_audio_io == NULL || terminal_capture_session_id != 0U) {
        return;
    }
    audio_io_status_t audio_status = {0};
    audio_io_get_status(terminal_audio_io, &audio_status);
    if (audio_status.playback_state == AUDIO_IO_PLAYBACK_STATE_BUFFERING ||
        audio_status.playback_state == AUDIO_IO_PLAYBACK_STATE_PLAYING) {
        const audio_io_result_t cancel_result = audio_io_playback_cancel(
            terminal_audio_io,
            audio_status.connection_epoch);
        if (cancel_result != AUDIO_IO_RESULT_OK &&
            cancel_result != AUDIO_IO_RESULT_INVALID_STATE) {
            (void)__atomic_fetch_add(
                &terminal_audio_cancel_error_count,
                1U,
                __ATOMIC_RELAXED);
            terminal_ui_project(TERMINAL_UI_STATE_ERROR, TERMINAL_UI_ERROR_AUDIO);
            return;
        }
        if (device_connection_send_playback_cancel(
                terminal_device_connection,
                audio_status.connection_epoch,
                audio_status.output_id,
                audio_status.stream_id) != DEVICE_CONNECTION_RESULT_OK) {
            (void)__atomic_fetch_add(
                &terminal_voice_input_error_count,
                1U,
                __ATOMIC_RELAXED);
        }
    }

    uint64_t capture_session_id = 0U;
    if (voice_input_begin(
            terminal_voice_input,
            terminal_online_connection_epoch,
            &capture_session_id) != VOICE_INPUT_RESULT_OK) {
        return;
    }
    const audio_io_result_t capture_result = audio_io_capture_begin(
        terminal_audio_io,
        capture_session_id);
    if (capture_result != AUDIO_IO_RESULT_OK) {
        (void)voice_input_cancel(terminal_voice_input, capture_session_id);
        (void)__atomic_fetch_add(
            &terminal_interaction_error_count,
            1U,
            __ATOMIC_RELAXED);
        terminal_ui_project(TERMINAL_UI_STATE_ERROR, TERMINAL_UI_ERROR_AUDIO);
        return;
    }
    terminal_capture_session_id = capture_session_id;
}

static void terminal_interaction_event_process(
    const terminal_interaction_event_t *event)
{
    if (event->type == TERMINAL_INTERACTION_EVENT_PTT_PRESSED) {
        terminal_ptt_pressed_process();
        return;
    }
    if (event->type == TERMINAL_INTERACTION_EVENT_PTT_RELEASED &&
        terminal_capture_session_id != 0U && terminal_audio_io != NULL) {
        const audio_io_result_t result = audio_io_capture_end(
            terminal_audio_io,
            terminal_capture_session_id);
        if (result != AUDIO_IO_RESULT_OK) {
            (void)__atomic_fetch_add(
                &terminal_interaction_error_count,
                1U,
                __ATOMIC_RELAXED);
        }
    }
}

static void terminal_voice_input_event_process(
    const voice_input_event_t *event)
{
    if (event->type == VOICE_INPUT_EVENT_SEALED ||
        event->type == VOICE_INPUT_EVENT_TRANSMISSION_STARTED ||
               event->type == VOICE_INPUT_EVENT_AWAITING_ACCEPTANCE ||
               event->type == VOICE_INPUT_EVENT_ACCEPTED) {
        terminal_ui_project(
            terminal_device_online ? TERMINAL_UI_STATE_THINKING
                                   : TERMINAL_UI_STATE_CONNECTING,
            TERMINAL_UI_ERROR_NONE);
    } else if (event->type == VOICE_INPUT_EVENT_REJECTED ||
               event->type == VOICE_INPUT_EVENT_CANCELLED) {
        terminal_ui_project(
            terminal_device_online ? TERMINAL_UI_STATE_READY
                                   : TERMINAL_UI_STATE_CONNECTING,
            TERMINAL_UI_ERROR_NONE);
    } else if (event->type == VOICE_INPUT_EVENT_EXPIRED ||
               event->type == VOICE_INPUT_EVENT_TOO_LONG ||
               event->type == VOICE_INPUT_EVENT_FAILED) {
        if (terminal_capture_session_id != 0U && terminal_audio_io != NULL) {
            (void)audio_io_capture_cancel(
                terminal_audio_io,
                terminal_capture_session_id);
        }
        terminal_ui_project(TERMINAL_UI_STATE_ERROR, TERMINAL_UI_ERROR_AUDIO);
    }
    ESP_LOGI(
        TAG,
        "event=voice_input type=%u result=%u frames=%llu samples=%llu retransmissions=%lu",
        (unsigned int)event->type,
        (unsigned int)event->result,
        (unsigned long long)event->frame_count,
        (unsigned long long)event->sample_count,
        (unsigned long)event->retransmission_count);
}

static void terminal_device_connection_network_changed(
    bool has_ip,
    uint64_t ip_epoch)
{
    if (terminal_device_connection == NULL) {
        return;
    }
    const device_connection_result_t result = device_connection_network_changed(
        terminal_device_connection,
        has_ip,
        ip_epoch);
    if (result != DEVICE_CONNECTION_RESULT_OK) {
        (void)__atomic_fetch_add(
            &terminal_device_network_update_error_count,
            1U,
            __ATOMIC_RELAXED);
    }
}

static void terminal_secure_zero(void *memory, size_t memory_size)
{
    volatile uint8_t *bytes = memory;
    while (memory_size > 0U) {
        *bytes++ = 0U;
        --memory_size;
    }
}

static bool terminal_task_is_observed(const char *task_name)
{
    for (size_t index = 0U;
         index < sizeof(terminal_observed_task_names) /
                     sizeof(terminal_observed_task_names[0]);
         ++index) {
        if (strncmp(
                task_name,
                terminal_observed_task_names[index],
                configMAX_TASK_NAME_LEN - 1U) == 0) {
            return true;
        }
    }
    return false;
}

static void terminal_resource_snapshot_log(void)
{
    const uint32_t internal_capabilities = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t external_capabilities = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI(
        TAG,
        "event=runtime_resource_snapshot internal_free_bytes=%lu "
        "internal_minimum_free_bytes=%lu internal_largest_block_bytes=%lu "
        "psram_free_bytes=%lu psram_minimum_free_bytes=%lu "
        "psram_largest_block_bytes=%lu",
        (unsigned long)heap_caps_get_free_size(internal_capabilities),
        (unsigned long)heap_caps_get_minimum_free_size(internal_capabilities),
        (unsigned long)heap_caps_get_largest_free_block(internal_capabilities),
        (unsigned long)heap_caps_get_free_size(external_capabilities),
        (unsigned long)heap_caps_get_minimum_free_size(external_capabilities),
        (unsigned long)heap_caps_get_largest_free_block(external_capabilities));

    TaskStatus_t task_statuses[TERMINAL_RESOURCE_MONITOR_MAX_TASKS];
    const UBaseType_t task_count = uxTaskGetSystemState(
        task_statuses,
        TERMINAL_RESOURCE_MONITOR_MAX_TASKS,
        NULL);
    if (task_count == 0U) {
        ESP_LOGW(TAG, "event=runtime_task_stack_snapshot status=unavailable");
        return;
    }
    for (UBaseType_t index = 0U; index < task_count; ++index) {
        if (!terminal_task_is_observed(task_statuses[index].pcTaskName)) {
            continue;
        }
        ESP_LOGI(
            TAG,
            "event=runtime_task_stack_snapshot task=%s minimum_free_bytes=%lu",
            task_statuses[index].pcTaskName,
            (unsigned long)task_statuses[index].usStackHighWaterMark);
    }
}

static void terminal_resource_monitor_task(void *context)
{
    (void)context;
    vTaskDelay(pdMS_TO_TICKS(TERMINAL_RESOURCE_MONITOR_INITIAL_DELAY_MS));
    while (true) {
        terminal_resource_snapshot_log();
        vTaskDelay(pdMS_TO_TICKS(TERMINAL_RESOURCE_MONITOR_INTERVAL_MS));
    }
}

static bool terminal_identity_load(
    device_connection_identity_t *connection_identity)
{
    device_identity_record_t identity_record = {0};
    const device_identity_result_t result = device_identity_load(
        terminal_device_identity,
        &identity_record);
    if (result == DEVICE_IDENTITY_RESULT_NOT_FOUND) {
        ESP_LOGI(TAG, "event=device_identity_loaded state=none host_binding_retained=false");
        return true;
    }
    if (result != DEVICE_IDENTITY_RESULT_OK) {
        ESP_LOGE(
            TAG,
            "event=device_identity_load_failed result=%u connection_service=disabled",
            (unsigned int)result);
        terminal_secure_zero(&identity_record, sizeof(identity_record));
        return false;
    }
    if (identity_record.state == DEVICE_IDENTITY_STATE_NONE) {
        ESP_LOGI(
            TAG,
            "event=device_identity_loaded state=none host_binding_retained=%s",
            identity_record.host_installation_id[0] == '\0' ? "false" : "true");
        terminal_secure_zero(&identity_record, sizeof(identity_record));
        return true;
    }
    connection_identity->paired = true;
    memcpy(
        connection_identity->installation_id,
        identity_record.host_installation_id,
        sizeof(connection_identity->installation_id));
    memcpy(
        connection_identity->certificate_fingerprint,
        identity_record.host_certificate_fingerprint,
        sizeof(connection_identity->certificate_fingerprint));
    ESP_LOGI(
        TAG,
        "event=device_identity_loaded state=%s",
        identity_record.state == DEVICE_IDENTITY_STATE_ACTIVE ? "active" : "pending");
    terminal_secure_zero(&identity_record, sizeof(identity_record));
    return true;
}

static void terminal_lifecycle_task(void *context)
{
    QueueHandle_t event_queue = context;
    terminal_event_t event;
    while (true) {
        if (xQueueReceive(event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        const uint32_t dropped =
            __atomic_exchange_n(&terminal_event_drop_count, 0U, __ATOMIC_RELAXED);
        if (dropped != 0U) {
            ESP_LOGE(TAG, "event=terminal_event_queue_overflow dropped=%lu", (unsigned long)dropped);
        }
        const uint32_t network_update_errors = __atomic_exchange_n(
            &terminal_device_network_update_error_count,
            0U,
            __ATOMIC_RELAXED);
        if (network_update_errors != 0U) {
            ESP_LOGE(
                TAG,
                "event=device_network_update_failed count=%lu",
                (unsigned long)network_update_errors);
        }
        const uint32_t ui_update_errors = __atomic_exchange_n(
            &terminal_ui_update_error_count,
            0U,
            __ATOMIC_RELAXED);
        if (ui_update_errors != 0U) {
            ESP_LOGE(
                TAG,
                "event=terminal_ui_update_failed count=%lu",
                (unsigned long)ui_update_errors);
        }
        const uint32_t audio_ingress_errors = __atomic_exchange_n(
            &terminal_audio_ingress_error_count,
            0U,
            __ATOMIC_RELAXED);
        if (audio_ingress_errors != 0U) {
            const uint32_t last_result = __atomic_exchange_n(
                &terminal_audio_ingress_last_result,
                (uint32_t)AUDIO_IO_RESULT_OK,
                __ATOMIC_RELAXED);
            ESP_LOGE(
                TAG,
                "event=audio_ingress_failed count=%lu last_result=%lu",
                (unsigned long)audio_ingress_errors,
                (unsigned long)last_result);
        }
        const uint32_t audio_cancel_errors = __atomic_exchange_n(
            &terminal_audio_cancel_error_count,
            0U,
            __ATOMIC_RELAXED);
        if (audio_cancel_errors != 0U) {
            ESP_LOGE(
                TAG,
                "event=audio_cancel_failed count=%lu",
                (unsigned long)audio_cancel_errors);
        }
        const uint32_t voice_input_errors = __atomic_exchange_n(
            &terminal_voice_input_error_count,
            0U,
            __ATOMIC_RELAXED);
        if (voice_input_errors != 0U) {
            ESP_LOGE(
                TAG,
                "event=voice_input_operation_failed count=%lu",
                (unsigned long)voice_input_errors);
        }
        const uint32_t interaction_errors = __atomic_exchange_n(
            &terminal_interaction_error_count,
            0U,
            __ATOMIC_RELAXED);
        if (interaction_errors != 0U) {
            ESP_LOGE(
                TAG,
                "event=terminal_interaction_failed count=%lu",
                (unsigned long)interaction_errors);
        }
        if (event.type == TERMINAL_EVENT_DEVICE_CONNECTION) {
            terminal_device_connection_event_process(&event.value.device_connection);
            continue;
        }
        if (event.type == TERMINAL_EVENT_DEVICE_MEDIA_STATE) {
            terminal_media_state_event_process(&event.value.media_state);
            continue;
        }
        if (event.type == TERMINAL_EVENT_AUDIO_IO) {
            terminal_audio_io_event_process(&event.value.audio_io);
            continue;
        }
        if (event.type == TERMINAL_EVENT_INTERACTION) {
            terminal_interaction_event_process(&event.value.interaction);
            continue;
        }
        if (event.type == TERMINAL_EVENT_VOICE_INPUT) {
            terminal_voice_input_event_process(&event.value.voice_input);
            continue;
        }
        const network_connectivity_event_t *network_event = &event.value.network;
        terminal_network_ui_project(network_event);
        if (network_event->type == NETWORK_CONNECTIVITY_EVENT_IP_ACQUIRED) {
            esp_ip4_addr_t address = {.addr = network_event->ipv4_address};
            ESP_LOGI(
                TAG,
                "event=network_ip_acquired state=%s ip=" IPSTR " ip_epoch=%llu",
                network_state_name(network_event->state),
                IP2STR(&address),
                (unsigned long long)network_event->ip_epoch);
        } else if (network_event->type == NETWORK_CONNECTIVITY_EVENT_IP_LOST) {
            ESP_LOGW(
                TAG,
                "event=network_ip_lost state=%s ip_epoch=%llu reason=%u",
                network_state_name(network_event->state),
                (unsigned long long)network_event->ip_epoch,
                network_event->disconnect_reason);
        } else {
            ESP_LOGI(
                TAG,
                "event=network_state_changed state=%s reconnect_delay_ms=%lu",
                network_state_name(network_event->state),
                (unsigned long)network_event->reconnect_delay_ms);
        }
    }
}

static void terminal_storage_initialize(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

static void terminal_boot_interaction_milestone(
    terminal_interaction_boot_milestone_t milestone,
    void *context)
{
    (void)context;
    if (milestone == TERMINAL_INTERACTION_BOOT_MILESTONE_PAIRING_RESET_ARMED) {
        terminal_ui_project(
            TERMINAL_UI_STATE_PAIRING_RESET_ARMED,
            TERMINAL_UI_ERROR_NONE);
    }
}

static bool terminal_boot_pairing_reset_evaluate(void)
{
    ESP_ERROR_CHECK(
        terminal_interaction_create(&terminal_interaction) ==
                TERMINAL_INTERACTION_RESULT_OK
            ? ESP_OK
            : ESP_FAIL);
    terminal_interaction_boot_action_t action = TERMINAL_INTERACTION_BOOT_ACTION_NONE;
    ESP_ERROR_CHECK(
        terminal_interaction_evaluate_boot_action(
            terminal_interaction,
            terminal_boot_interaction_milestone,
            NULL,
            &action) == TERMINAL_INTERACTION_RESULT_OK
            ? ESP_OK
            : ESP_FAIL);
    if (action != TERMINAL_INTERACTION_BOOT_ACTION_CLEAR_PAIRING) {
        return false;
    }
    if (device_identity_clear_all(terminal_device_identity) != DEVICE_IDENTITY_RESULT_OK) {
        ESP_LOGE(TAG, "event=pairing_reset_failed reason=identity_storage");
        terminal_ui_project(TERMINAL_UI_STATE_ERROR, TERMINAL_UI_ERROR_IDENTITY);
        return true;
    }
    ESP_LOGI(TAG, "event=pairing_reset_completed restart=true");
    vTaskDelay(pdMS_TO_TICKS(250U));
    esp_restart();
    return true;
}

void app_main(void)
{
    terminal_storage_initialize();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(
        terminal_ui_create(&terminal_user_interface) == TERMINAL_UI_RESULT_OK
            ? ESP_OK
            : ESP_FAIL);
    ESP_ERROR_CHECK(
        terminal_ui_start(terminal_user_interface) == TERMINAL_UI_RESULT_OK
            ? ESP_OK
            : ESP_FAIL);
    ESP_ERROR_CHECK(
        device_identity_open(&terminal_device_identity) == DEVICE_IDENTITY_RESULT_OK
            ? ESP_OK
            : ESP_FAIL);
    if (terminal_boot_pairing_reset_evaluate()) {
        return;
    }

    terminal_event_queue = xQueueCreate(
        TERMINAL_EVENT_QUEUE_LENGTH,
        sizeof(terminal_event_t));
    ESP_ERROR_CHECK(terminal_event_queue == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    ESP_ERROR_CHECK(
        xTaskCreate(
            terminal_lifecycle_task,
            "terminal_lifecycle",
            TERMINAL_LIFECYCLE_TASK_STACK_BYTES,
            terminal_event_queue,
            TERMINAL_LIFECYCLE_TASK_PRIORITY,
            NULL)
                == pdPASS
            ? ESP_OK
            : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(
        xTaskCreate(
            terminal_resource_monitor_task,
            "terminal_resources",
            TERMINAL_RESOURCE_MONITOR_TASK_STACK_BYTES,
            NULL,
            TERMINAL_RESOURCE_MONITOR_TASK_PRIORITY,
            NULL)
                == pdPASS
            ? ESP_OK
            : ESP_ERR_NO_MEM);

    const audio_io_config_t audio_io_config = {
        .playback_gain_percent = AUDIO_IO_DEFAULT_PLAYBACK_GAIN_PERCENT,
        .event_callback = terminal_audio_io_event,
        .event_context = terminal_event_queue,
        .capture_frame_callback = terminal_audio_capture_frame,
        .capture_frame_context = NULL,
    };
    ESP_ERROR_CHECK(
        audio_io_create(&audio_io_config, &terminal_audio_io) == AUDIO_IO_RESULT_OK
            ? ESP_OK
            : ESP_FAIL);
    ESP_ERROR_CHECK(
        audio_io_start(terminal_audio_io) == AUDIO_IO_RESULT_OK
            ? ESP_OK
            : ESP_FAIL);

    device_connection_identity_t connection_identity = {0};
    const bool identity_loaded = terminal_identity_load(&connection_identity);
    if (identity_loaded) {
        terminal_device_paired = connection_identity.paired;
        const device_connection_config_t device_connection_config = {
            .identity = connection_identity,
            .identity_store = terminal_device_identity,
            .capabilities = {
                .input_text = false,
                .input_pcm16_16k_mono = true,
                .output_text = false,
                .output_pcm16_16k_mono = true,
                .playback_cancel = true,
                .display_status = true,
                .display_transcript = false,
            },
            .output_preference = DEVICE_OUTPUT_PREFERENCE_AUDIO,
            .client_version = esp_app_get_description()->version,
            .display_name = "Agent Robot Terminal",
            .event_callback = terminal_device_connection_event,
            .event_context = terminal_event_queue,
            .media_callback = terminal_device_media_event,
            .media_context = terminal_event_queue,
        };
        ESP_ERROR_CHECK(
            device_connection_create(
                &device_connection_config,
                &terminal_device_connection) == DEVICE_CONNECTION_RESULT_OK
                ? ESP_OK
                : ESP_FAIL);
        ESP_ERROR_CHECK(
            device_connection_start(terminal_device_connection) == DEVICE_CONNECTION_RESULT_OK
                ? ESP_OK
                : ESP_FAIL);
        const voice_input_config_t voice_input_config = {
            .connection = terminal_device_connection,
            .output_preference = DEVICE_OUTPUT_PREFERENCE_AUDIO,
            .event_callback = terminal_voice_input_event,
            .event_context = terminal_event_queue,
        };
        ESP_ERROR_CHECK(
            voice_input_create(&voice_input_config, &terminal_voice_input) ==
                    VOICE_INPUT_RESULT_OK
                ? ESP_OK
                : ESP_FAIL);
        ESP_ERROR_CHECK(
            voice_input_start(terminal_voice_input) == VOICE_INPUT_RESULT_OK
                ? ESP_OK
                : ESP_FAIL);
        ESP_ERROR_CHECK(
            terminal_interaction_start(
                terminal_interaction,
                terminal_interaction_event,
                terminal_event_queue) == TERMINAL_INTERACTION_RESULT_OK
                ? ESP_OK
                : ESP_FAIL);
        terminal_ui_project(terminal_connection_ui_state(), TERMINAL_UI_ERROR_NONE);
    } else {
        terminal_ui_project(TERMINAL_UI_STATE_ERROR, TERMINAL_UI_ERROR_IDENTITY);
    }

    const network_connectivity_config_t network_config = {
        .event_callback = terminal_network_event,
        .event_context = terminal_event_queue,
    };
    ESP_ERROR_CHECK(
        network_connectivity_create(
            &network_config,
            &terminal_network_connectivity)
                == NETWORK_CONNECTIVITY_RESULT_OK
            ? ESP_OK
            : ESP_FAIL);
    ESP_ERROR_CHECK(
        network_connectivity_start(terminal_network_connectivity)
                == NETWORK_CONNECTIVITY_RESULT_OK
            ? ESP_OK
            : ESP_FAIL);
    ESP_LOGI(
        TAG,
        "event=terminal_boot status=%s",
        identity_loaded ? "connection_services_started" : "identity_recovery_required");
}
