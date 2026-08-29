#include "device_connection.h"

#include <stdlib.h>
#include <string.h>

#include "device_control_envelope.h"
#include "device_message_replay_window.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define DEVICE_CONNECTION_TASK_STACK_BYTES 12288U
#define DEVICE_CONNECTION_TASK_PRIORITY 6U
#define DEVICE_CONNECTION_DISCOVERY_TIMEOUT_MS 5000U
#define DEVICE_CONNECTION_TCP_TIMEOUT_MS 5000U
#define DEVICE_CONNECTION_TLS_TIMEOUT_MS 10000U
#define DEVICE_CONNECTION_UPGRADE_TIMEOUT_MS 10000U
#define DEVICE_CONNECTION_RECEIVE_TIMEOUT_MS 30000U
#define DEVICE_CONNECTION_STOPPED_BIT BIT0

typedef struct {
    bool network_has_ip;
    uint64_t network_ip_epoch;
    uint64_t identity_revision;
    bool stop_requested;
    device_connection_identity_t identity;
} device_connection_inputs_t;

struct device_connection {
    portMUX_TYPE input_lock;
    device_connection_inputs_t inputs;
    device_connection_event_callback_t event_callback;
    void *event_context;
    TaskHandle_t task;
    EventGroupHandle_t lifecycle_events;
    bool started;
    device_connection_status_t status;
    device_connection_supervisor_t supervisor;
    wss_transport_t *transport;
    uint8_t *receive_payload;
    host_discovery_record_t cached_host;
    bool has_cached_host;
    uint64_t observed_ip_epoch;
    uint64_t observed_identity_revision;
    device_connection_identity_t identity;
    device_message_replay_window_t *replay_window;
};

static uint64_t monotonic_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static device_connection_inputs_t input_snapshot(device_connection_t *connection)
{
    device_connection_inputs_t snapshot;
    taskENTER_CRITICAL(&connection->input_lock);
    snapshot = connection->inputs;
    taskEXIT_CRITICAL(&connection->input_lock);
    return snapshot;
}

static bool transport_cancelled(void *context)
{
    device_connection_t *connection = context;
    const device_connection_inputs_t inputs = input_snapshot(connection);
    return inputs.stop_requested || !inputs.network_has_ip ||
           inputs.network_ip_epoch != connection->observed_ip_epoch ||
           inputs.identity_revision != connection->observed_identity_revision;
}

static void copy_status(device_connection_t *connection)
{
    taskENTER_CRITICAL(&connection->input_lock);
    connection->status.state = connection->supervisor.state;
    connection->status.block_reason = connection->supervisor.block_reason;
    connection->status.connection_epoch = connection->supervisor.connection_epoch;
    connection->status.consecutive_failures = connection->supervisor.consecutive_failures;
    taskEXIT_CRITICAL(&connection->input_lock);
}

static void emit_event(
    device_connection_t *connection,
    device_connection_event_type_t type,
    device_connection_failure_t failure,
    host_selection_result_t selection_result,
    wss_transport_result_t transport_result,
    const char *message_id,
    const char *message_type)
{
    copy_status(connection);
    if (connection->event_callback == NULL) {
        return;
    }
    device_connection_event_t event = {
        .type = type,
        .state = connection->supervisor.state,
        .block_reason = connection->supervisor.block_reason,
        .failure = failure,
        .selection_result = selection_result,
        .transport_result = transport_result,
        .network_ip_epoch = connection->observed_ip_epoch,
        .connection_epoch = connection->supervisor.connection_epoch,
    };
    if (connection->supervisor.state == DEVICE_CONNECTION_STATE_BACKOFF) {
        const uint64_t now_ms = monotonic_ms();
        if (connection->supervisor.backoff_until_ms > now_ms) {
            const uint64_t remaining = connection->supervisor.backoff_until_ms - now_ms;
            event.reconnect_delay_ms = remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
        }
    }
    if (connection->has_cached_host) {
        const size_t installation_length = strlen(connection->cached_host.installation_id);
        if (installation_length >= sizeof(event.installation_id_suffix) - 1U) {
            memcpy(
                event.installation_id_suffix,
                connection->cached_host.installation_id + installation_length -
                    (sizeof(event.installation_id_suffix) - 1U),
                sizeof(event.installation_id_suffix) - 1U);
        }
    }
    if (message_id != NULL) {
        strncpy(event.message_id, message_id, sizeof(event.message_id) - 1U);
    }
    if (message_type != NULL) {
        strncpy(event.message_type, message_type, sizeof(event.message_type) - 1U);
    }
    connection->event_callback(&event, connection->event_context);
}

static void emit_state_changed(device_connection_t *connection)
{
    emit_event(
        connection,
        DEVICE_CONNECTION_EVENT_STATE_CHANGED,
        DEVICE_CONNECTION_FAILURE_TRANSIENT,
        HOST_SELECTION_NOT_FOUND,
        WSS_TRANSPORT_RESULT_OK,
        NULL,
        NULL);
}

static bool copy_transport_endpoint(
    const host_discovery_record_t *host,
    wss_transport_endpoint_t *endpoint)
{
    const size_t path_length = strlen(host->path);
    const size_t fingerprint_length = strlen(host->certificate_fingerprint);
    if (path_length >= sizeof(endpoint->path) ||
        fingerprint_length >= sizeof(endpoint->certificate_fingerprint)) {
        return false;
    }
    memset(endpoint, 0, sizeof(*endpoint));
    endpoint->ipv4_address = host->ipv4_address;
    endpoint->port = host->port;
    memcpy(endpoint->path, host->path, path_length + 1U);
    memcpy(
        endpoint->certificate_fingerprint,
        host->certificate_fingerprint,
        fingerprint_length + 1U);
    return true;
}

static device_connection_failure_t transport_failure(wss_transport_result_t result)
{
    switch (result) {
        case WSS_TRANSPORT_RESULT_CERTIFICATE_UNAVAILABLE:
        case WSS_TRANSPORT_RESULT_CERTIFICATE_MISMATCH:
            return DEVICE_CONNECTION_FAILURE_CERTIFICATE;
        case WSS_TRANSPORT_RESULT_INVALID_ARGUMENT:
        case WSS_TRANSPORT_RESULT_UPGRADE_REJECTED:
        case WSS_TRANSPORT_RESULT_PROTOCOL_ERROR:
        case WSS_TRANSPORT_RESULT_CAPACITY_EXCEEDED:
            return DEVICE_CONNECTION_FAILURE_PROTOCOL;
        default:
            return DEVICE_CONNECTION_FAILURE_TRANSIENT;
    }
}

static void record_failure(
    device_connection_t *connection,
    device_connection_failure_t failure,
    wss_transport_result_t transport_result)
{
    const uint64_t epoch = connection->supervisor.connection_epoch;
    if (device_connection_supervisor_fail(
            &connection->supervisor,
            epoch,
            failure,
            monotonic_ms(),
            esp_random())) {
        emit_event(
            connection,
            DEVICE_CONNECTION_EVENT_FAILURE,
            failure,
            HOST_SELECTION_NOT_FOUND,
            transport_result,
            NULL,
            NULL);
    }
}

static void record_discovery_failure(
    device_connection_t *connection,
    device_connection_failure_t failure,
    host_selection_result_t selection_result)
{
    if (device_connection_supervisor_discovery_failed(
            &connection->supervisor,
            failure,
            monotonic_ms(),
            esp_random())) {
        emit_event(
            connection,
            DEVICE_CONNECTION_EVENT_FAILURE,
            failure,
            selection_result,
            WSS_TRANSPORT_RESULT_OK,
            NULL,
            NULL);
    }
}

static void synchronize_inputs(device_connection_t *connection)
{
    const device_connection_inputs_t inputs = input_snapshot(connection);
    const bool identity_changed =
        inputs.identity_revision != connection->observed_identity_revision;
    const bool network_changed =
        inputs.network_ip_epoch != connection->observed_ip_epoch;
    if ((!inputs.network_has_ip || network_changed || identity_changed) &&
        wss_transport_is_connected(connection->transport)) {
        wss_transport_disconnect(connection->transport);
    }
    if (!inputs.network_has_ip) {
        if (connection->supervisor.wifi_has_ip) {
            device_connection_supervisor_wifi_down(&connection->supervisor, monotonic_ms());
            connection->has_cached_host = false;
            emit_state_changed(connection);
        }
        connection->observed_ip_epoch = inputs.network_ip_epoch;
        connection->observed_identity_revision = inputs.identity_revision;
        connection->identity = inputs.identity;
        return;
    }
    if (!connection->supervisor.wifi_has_ip || network_changed || identity_changed) {
        if (identity_changed) {
            device_connection_supervisor_identity_changed(&connection->supervisor);
        }
        if (connection->supervisor.wifi_has_ip) {
            device_connection_supervisor_wifi_down(&connection->supervisor, monotonic_ms());
        }
        connection->observed_ip_epoch = inputs.network_ip_epoch;
        connection->observed_identity_revision = inputs.identity_revision;
        connection->identity = inputs.identity;
        connection->has_cached_host = false;
        device_connection_supervisor_got_ip(&connection->supervisor, monotonic_ms());
        emit_state_changed(connection);
    }
}

static void discover_host(device_connection_t *connection)
{
    taskENTER_CRITICAL(&connection->input_lock);
    ++connection->status.discovery_attempt_count;
    taskEXIT_CRITICAL(&connection->input_lock);

    host_discovery_record_t records[HOST_DISCOVERY_MAX_RESULTS];
    size_t record_count = 0U;
    const host_discovery_result_t query_result = host_discovery_query(
        DEVICE_CONNECTION_DISCOVERY_TIMEOUT_MS,
        records,
        HOST_DISCOVERY_MAX_RESULTS,
        &record_count);
    if (transport_cancelled(connection)) {
        return;
    }
    if (query_result != HOST_DISCOVERY_RESULT_OK) {
        record_discovery_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_TRANSIENT,
            HOST_SELECTION_NOT_FOUND);
        return;
    }

    host_selection_identity_t selection_identity = {
        .paired = connection->identity.paired,
    };
    memcpy(
        selection_identity.installation_id,
        connection->identity.installation_id,
        sizeof(selection_identity.installation_id));
    memcpy(
        selection_identity.certificate_fingerprint,
        connection->identity.certificate_fingerprint,
        sizeof(selection_identity.certificate_fingerprint));
    host_discovery_record_t selected;
    const host_selection_result_t selection_result = host_discovery_select(
        records,
        record_count,
        &selection_identity,
        &selected);
    if (selection_result != HOST_SELECTION_SELECTED) {
        const device_connection_failure_t failure =
            selection_result == HOST_SELECTION_CERTIFICATE_CHANGED
                ? DEVICE_CONNECTION_FAILURE_CERTIFICATE
                : DEVICE_CONNECTION_FAILURE_SERVICE_UNAVAILABLE;
        record_discovery_failure(connection, failure, selection_result);
        return;
    }

    connection->cached_host = selected;
    connection->has_cached_host = true;
    emit_event(
        connection,
        DEVICE_CONNECTION_EVENT_HOST_SELECTED,
        DEVICE_CONNECTION_FAILURE_TRANSIENT,
        selection_result,
        WSS_TRANSPORT_RESULT_OK,
        NULL,
        NULL);
    uint64_t connection_epoch = 0U;
    if (device_connection_supervisor_connect_discovered(
            &connection->supervisor,
            &connection_epoch)) {
        device_message_replay_window_initialize(connection->replay_window);
        emit_state_changed(connection);
    }
}

static void connect_transport(device_connection_t *connection)
{
    if (!connection->has_cached_host) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_TRANSIENT,
            WSS_TRANSPORT_RESULT_INVALID_ARGUMENT);
        return;
    }
    wss_transport_endpoint_t endpoint;
    if (!copy_transport_endpoint(&connection->cached_host, &endpoint)) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_INVALID_ARGUMENT);
        return;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    ++connection->status.transport_attempt_count;
    taskEXIT_CRITICAL(&connection->input_lock);
    const wss_transport_result_t result = wss_transport_connect(
        connection->transport,
        &endpoint,
        DEVICE_CONNECTION_TCP_TIMEOUT_MS,
        DEVICE_CONNECTION_TLS_TIMEOUT_MS,
        DEVICE_CONNECTION_UPGRADE_TIMEOUT_MS);
    if (result == WSS_TRANSPORT_RESULT_CANCELLED || transport_cancelled(connection)) {
        return;
    }
    if (result != WSS_TRANSPORT_RESULT_OK) {
        record_failure(connection, transport_failure(result), result);
        return;
    }
    if (device_connection_supervisor_transport_ready(
            &connection->supervisor,
            connection->supervisor.connection_epoch)) {
        emit_state_changed(connection);
    }
}

static void receive_transport_message(device_connection_t *connection)
{
    wss_transport_message_t message;
    const wss_transport_result_t result = wss_transport_receive(
        connection->transport,
        connection->receive_payload,
        WSS_TRANSPORT_CONTROL_MAX_BYTES,
        &message,
        DEVICE_CONNECTION_RECEIVE_TIMEOUT_MS);
    if (result == WSS_TRANSPORT_RESULT_CANCELLED || transport_cancelled(connection)) {
        return;
    }
    if (result != WSS_TRANSPORT_RESULT_OK) {
        record_failure(connection, transport_failure(result), result);
        return;
    }
    if (message.type == WSS_TRANSPORT_MESSAGE_PING ||
        message.type == WSS_TRANSPORT_MESSAGE_PONG) {
        return;
    }
    if (message.type == WSS_TRANSPORT_MESSAGE_CLOSE) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_TRANSIENT,
            WSS_TRANSPORT_RESULT_CONNECTION_CLOSED);
        return;
    }
    if (message.type != WSS_TRANSPORT_MESSAGE_TEXT) {
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }

    device_control_envelope_t *envelope = NULL;
    const device_protocol_result_t decode_result = device_control_envelope_decode(
        (const char *)connection->receive_payload,
        message.payload_length,
        &envelope);
    if (decode_result != DEVICE_PROTOCOL_RESULT_OK || envelope == NULL ||
        device_message_replay_window_accept(
            connection->replay_window,
            device_control_envelope_message_id(envelope)) != DEVICE_PROTOCOL_RESULT_OK) {
        device_control_envelope_release(envelope);
        record_failure(
            connection,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            WSS_TRANSPORT_RESULT_PROTOCOL_ERROR);
        return;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    ++connection->status.control_message_count;
    taskEXIT_CRITICAL(&connection->input_lock);
    emit_event(
        connection,
        DEVICE_CONNECTION_EVENT_CONTROL_RECEIVED,
        DEVICE_CONNECTION_FAILURE_TRANSIENT,
        HOST_SELECTION_SELECTED,
        WSS_TRANSPORT_RESULT_OK,
        device_control_envelope_message_id(envelope),
        device_control_envelope_message_type(envelope));
    device_control_envelope_release(envelope);
}

static void wait_for_work(device_connection_t *connection)
{
    TickType_t wait_ticks = portMAX_DELAY;
    if (connection->supervisor.state == DEVICE_CONNECTION_STATE_BACKOFF) {
        const uint64_t now_ms = monotonic_ms();
        if (connection->supervisor.backoff_until_ms <= now_ms) {
            wait_ticks = 0U;
        } else {
            const uint64_t remaining_ms = connection->supervisor.backoff_until_ms - now_ms;
            wait_ticks = pdMS_TO_TICKS(
                remaining_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining_ms);
        }
    }
    (void)ulTaskNotifyTake(pdTRUE, wait_ticks);
}

static void device_connection_task(void *context)
{
    device_connection_t *connection = context;
    const host_discovery_result_t discovery_result = host_discovery_initialize();
    const wss_transport_result_t transport_result =
        discovery_result == HOST_DISCOVERY_RESULT_OK
            ? wss_transport_create(&connection->transport)
            : WSS_TRANSPORT_RESULT_NETWORK_UNAVAILABLE;
    connection->receive_payload = heap_caps_malloc(
        WSS_TRANSPORT_CONTROL_MAX_BYTES,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    connection->replay_window = heap_caps_malloc(
        sizeof(device_message_replay_window_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const bool resources_ready = discovery_result == HOST_DISCOVERY_RESULT_OK &&
                                 transport_result == WSS_TRANSPORT_RESULT_OK &&
                                 connection->receive_payload != NULL &&
                                 connection->replay_window != NULL;
    if (!resources_ready) {
        connection->supervisor.state = DEVICE_CONNECTION_STATE_BLOCKED;
        connection->supervisor.block_reason = DEVICE_CONNECTION_BLOCK_PROTOCOL;
        emit_event(
            connection,
            DEVICE_CONNECTION_EVENT_FAILURE,
            DEVICE_CONNECTION_FAILURE_PROTOCOL,
            HOST_SELECTION_NOT_FOUND,
            transport_result,
            NULL,
            NULL);
    } else {
        wss_transport_set_cancel_callback(
            connection->transport,
            transport_cancelled,
            connection);
    }

    while (!resources_ready) {
        if (input_snapshot(connection).stop_requested) {
            break;
        }
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }

    while (resources_ready) {
        const device_connection_inputs_t inputs = input_snapshot(connection);
        if (inputs.stop_requested) {
            break;
        }
        synchronize_inputs(connection);
        if (!connection->supervisor.wifi_has_ip) {
            wait_for_work(connection);
            continue;
        }
        switch (connection->supervisor.state) {
            case DEVICE_CONNECTION_STATE_DISCOVERING:
                discover_host(connection);
                break;
            case DEVICE_CONNECTION_STATE_CONNECTING:
                connect_transport(connection);
                break;
            case DEVICE_CONNECTION_STATE_AUTHENTICATING:
            case DEVICE_CONNECTION_STATE_ONLINE:
                receive_transport_message(connection);
                break;
            case DEVICE_CONNECTION_STATE_BACKOFF: {
                wait_for_work(connection);
                synchronize_inputs(connection);
                uint64_t connection_epoch = 0U;
                const device_connection_action_t action = device_connection_supervisor_poll(
                    &connection->supervisor,
                    monotonic_ms(),
                    esp_random(),
                    &connection_epoch);
                if (action != DEVICE_CONNECTION_ACTION_NONE) {
                    emit_state_changed(connection);
                }
                break;
            }
            case DEVICE_CONNECTION_STATE_BLOCKED:
                wait_for_work(connection);
                break;
            default:
                wait_for_work(connection);
                break;
        }
    }

    if (connection->transport != NULL) {
        wss_transport_destroy(connection->transport);
        connection->transport = NULL;
    }
    free(connection->receive_payload);
    connection->receive_payload = NULL;
    free(connection->replay_window);
    connection->replay_window = NULL;
    if (discovery_result == HOST_DISCOVERY_RESULT_OK) {
        host_discovery_deinitialize();
    }
    taskENTER_CRITICAL(&connection->input_lock);
    connection->started = false;
    connection->task = NULL;
    taskEXIT_CRITICAL(&connection->input_lock);
    xEventGroupSetBits(connection->lifecycle_events, DEVICE_CONNECTION_STOPPED_BIT);
    vTaskDelete(NULL);
}

static bool identity_valid(const device_connection_identity_t *identity)
{
    if (identity == NULL) {
        return false;
    }
    if (!identity->paired) {
        return identity->installation_id[0] == '\0' &&
               identity->certificate_fingerprint[0] == '\0';
    }
    host_selection_identity_t selection_identity = {.paired = true};
    memcpy(
        selection_identity.installation_id,
        identity->installation_id,
        sizeof(selection_identity.installation_id));
    memcpy(
        selection_identity.certificate_fingerprint,
        identity->certificate_fingerprint,
        sizeof(selection_identity.certificate_fingerprint));
    host_discovery_record_t selected;
    return host_discovery_select(NULL, 0U, &selection_identity, &selected) ==
           HOST_SELECTION_NOT_FOUND;
}

device_connection_result_t device_connection_create(
    const device_connection_config_t *config,
    device_connection_t **connection)
{
    if (config == NULL || connection == NULL || !identity_valid(&config->identity)) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    *connection = calloc(1U, sizeof(device_connection_t));
    if (*connection == NULL) {
        return DEVICE_CONNECTION_RESULT_NO_MEMORY;
    }
    (*connection)->input_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    (*connection)->inputs.identity = config->identity;
    (*connection)->identity = config->identity;
    (*connection)->event_callback = config->event_callback;
    (*connection)->event_context = config->event_context;
    (*connection)->lifecycle_events = xEventGroupCreate();
    if ((*connection)->lifecycle_events == NULL) {
        free(*connection);
        *connection = NULL;
        return DEVICE_CONNECTION_RESULT_NO_MEMORY;
    }
    device_connection_supervisor_initialize(&(*connection)->supervisor);
    (*connection)->status.state = DEVICE_CONNECTION_STATE_WIFI_DOWN;
    return DEVICE_CONNECTION_RESULT_OK;
}

device_connection_result_t device_connection_start(device_connection_t *connection)
{
    if (connection == NULL) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    if (connection->started) {
        taskEXIT_CRITICAL(&connection->input_lock);
        return DEVICE_CONNECTION_RESULT_INVALID_STATE;
    }
    connection->inputs.stop_requested = false;
    connection->started = true;
    taskEXIT_CRITICAL(&connection->input_lock);
    xEventGroupClearBits(connection->lifecycle_events, DEVICE_CONNECTION_STOPPED_BIT);
    if (xTaskCreate(
            device_connection_task,
            "device_connection",
            DEVICE_CONNECTION_TASK_STACK_BYTES,
            connection,
            DEVICE_CONNECTION_TASK_PRIORITY,
            &connection->task) != pdPASS) {
        taskENTER_CRITICAL(&connection->input_lock);
        connection->started = false;
        taskEXIT_CRITICAL(&connection->input_lock);
        return DEVICE_CONNECTION_RESULT_NO_MEMORY;
    }
    return DEVICE_CONNECTION_RESULT_OK;
}

device_connection_result_t device_connection_network_changed(
    device_connection_t *connection,
    bool has_ip,
    uint64_t ip_epoch)
{
    if (connection == NULL || (has_ip && ip_epoch == 0U)) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    if (!connection->started) {
        taskEXIT_CRITICAL(&connection->input_lock);
        return DEVICE_CONNECTION_RESULT_INVALID_STATE;
    }
    connection->inputs.network_has_ip = has_ip;
    connection->inputs.network_ip_epoch = ip_epoch;
    connection->status.network_has_ip = has_ip;
    connection->status.network_ip_epoch = ip_epoch;
    TaskHandle_t task = connection->task;
    taskEXIT_CRITICAL(&connection->input_lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    return DEVICE_CONNECTION_RESULT_OK;
}

device_connection_result_t device_connection_update_identity(
    device_connection_t *connection,
    const device_connection_identity_t *identity)
{
    if (connection == NULL || !identity_valid(identity)) {
        return DEVICE_CONNECTION_RESULT_INVALID_ARGUMENT;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    if (memcmp(&connection->inputs.identity, identity, sizeof(*identity)) == 0) {
        taskEXIT_CRITICAL(&connection->input_lock);
        return DEVICE_CONNECTION_RESULT_OK;
    }
    connection->inputs.identity = *identity;
    ++connection->inputs.identity_revision;
    if (connection->inputs.identity_revision == 0U) {
        connection->inputs.identity_revision = 1U;
    }
    TaskHandle_t task = connection->task;
    taskEXIT_CRITICAL(&connection->input_lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    return DEVICE_CONNECTION_RESULT_OK;
}

void device_connection_get_status(
    device_connection_t *connection,
    device_connection_status_t *status)
{
    if (connection == NULL || status == NULL) {
        return;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    *status = connection->status;
    taskEXIT_CRITICAL(&connection->input_lock);
}

void device_connection_stop(device_connection_t *connection)
{
    if (connection == NULL) {
        return;
    }
    taskENTER_CRITICAL(&connection->input_lock);
    const bool started = connection->started;
    connection->inputs.stop_requested = true;
    connection->inputs.network_has_ip = false;
    TaskHandle_t task = connection->task;
    taskEXIT_CRITICAL(&connection->input_lock);
    if (!started) {
        return;
    }
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    (void)xEventGroupWaitBits(
        connection->lifecycle_events,
        DEVICE_CONNECTION_STOPPED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY);
}

void device_connection_destroy(device_connection_t *connection)
{
    if (connection == NULL) {
        return;
    }
    device_connection_stop(connection);
    vEventGroupDelete(connection->lifecycle_events);
    free(connection);
}
