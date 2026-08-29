#include "network_connectivity.h"

#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "network_reconnect_policy.h"
#include "nvs.h"

#define NETWORK_CREDENTIAL_NAMESPACE "wifi_cfg"
#define NETWORK_CREDENTIAL_SSID_KEY "ssid"
#define NETWORK_CREDENTIAL_PASSWORD_KEY "password"
#define NETWORK_SSID_CAPACITY 33U
#define NETWORK_PASSWORD_CAPACITY 64U

struct network_connectivity {
    network_connectivity_config_t config;
    portMUX_TYPE lock;
    esp_timer_handle_t reconnect_timer;
    esp_event_handler_instance_t wifi_event_instance;
    esp_event_handler_instance_t ip_event_instance;
    esp_netif_t *station_netif;
    network_connectivity_status_t status;
    bool started;
    bool wifi_initialized;
    bool handlers_registered;
};

static uint64_t next_epoch(uint64_t current)
{
    ++current;
    return current == 0U ? 1U : current;
}

static network_connectivity_event_t event_snapshot(
    network_connectivity_t *connectivity,
    network_connectivity_event_type_t type)
{
    network_connectivity_event_t event;
    portENTER_CRITICAL(&connectivity->lock);
    event.type = type;
    event.state = connectivity->status.state;
    event.ip_epoch = connectivity->status.ip_epoch;
    event.ipv4_address = connectivity->status.ipv4_address;
    event.reconnect_delay_ms = connectivity->status.current_reconnect_delay_ms;
    event.disconnect_reason = connectivity->status.last_disconnect_reason;
    portEXIT_CRITICAL(&connectivity->lock);
    return event;
}

static void publish_event(
    network_connectivity_t *connectivity,
    network_connectivity_event_type_t type)
{
    if (connectivity->config.event_callback == NULL) {
        return;
    }
    const network_connectivity_event_t event = event_snapshot(connectivity, type);
    connectivity->config.event_callback(&event, connectivity->config.event_context);
}

static void set_state(
    network_connectivity_t *connectivity,
    network_connectivity_state_t state)
{
    bool changed = false;
    portENTER_CRITICAL(&connectivity->lock);
    if (connectivity->status.state != state) {
        connectivity->status.state = state;
        changed = true;
    }
    portEXIT_CRITICAL(&connectivity->lock);
    if (changed) {
        publish_event(connectivity, NETWORK_CONNECTIVITY_EVENT_STATE_CHANGED);
    }
}

static esp_err_t load_credentials(
    char ssid[NETWORK_SSID_CAPACITY],
    char password[NETWORK_PASSWORD_CAPACITY])
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(NETWORK_CREDENTIAL_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        return result;
    }
    size_t stored_ssid_length = NETWORK_SSID_CAPACITY;
    result = nvs_get_str(handle, NETWORK_CREDENTIAL_SSID_KEY, ssid, &stored_ssid_length);
    if (result == ESP_OK) {
        size_t password_length = NETWORK_PASSWORD_CAPACITY;
        result = nvs_get_str(
            handle, NETWORK_CREDENTIAL_PASSWORD_KEY, password, &password_length);
    }
    nvs_close(handle);
    if (result != ESP_OK) {
        return result;
    }
    const size_t ssid_length = strlen(ssid);
    const size_t password_length = strlen(password);
    if (ssid_length == 0U || ssid_length > 32U || password_length > 63U ||
        (password_length > 0U && password_length < 8U)) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static bool is_started(network_connectivity_t *connectivity)
{
    bool started;
    portENTER_CRITICAL(&connectivity->lock);
    started = connectivity->started;
    portEXIT_CRITICAL(&connectivity->lock);
    return started;
}

static void mark_start_failed(network_connectivity_t *connectivity)
{
    portENTER_CRITICAL(&connectivity->lock);
    connectivity->started = false;
    portEXIT_CRITICAL(&connectivity->lock);
    set_state(connectivity, NETWORK_CONNECTIVITY_STATE_ERROR);
}

static void schedule_reconnect(network_connectivity_t *connectivity)
{
    uint32_t reconnect_delay_ms;
    portENTER_CRITICAL(&connectivity->lock);
    if (!connectivity->started || !connectivity->status.has_credentials) {
        portEXIT_CRITICAL(&connectivity->lock);
        return;
    }
    if (connectivity->status.reconnect_attempt < UINT32_MAX) {
        ++connectivity->status.reconnect_attempt;
    }
    reconnect_delay_ms = network_reconnect_policy_delay_ms(
        connectivity->status.reconnect_attempt);
    connectivity->status.current_reconnect_delay_ms = reconnect_delay_ms;
    connectivity->status.state = NETWORK_CONNECTIVITY_STATE_BACKOFF;
    portEXIT_CRITICAL(&connectivity->lock);

    publish_event(connectivity, NETWORK_CONNECTIVITY_EVENT_STATE_CHANGED);
    (void)esp_timer_stop(connectivity->reconnect_timer);
    if (esp_timer_start_once(
            connectivity->reconnect_timer, (uint64_t)reconnect_delay_ms * 1000U) != ESP_OK) {
        set_state(connectivity, NETWORK_CONNECTIVITY_STATE_ERROR);
    }
}

static void reconnect_timer_callback(void *context)
{
    network_connectivity_t *connectivity = context;
    if (!is_started(connectivity)) {
        return;
    }
    set_state(connectivity, NETWORK_CONNECTIVITY_STATE_CONNECTING);
    if (esp_wifi_connect() != ESP_OK) {
        schedule_reconnect(connectivity);
    }
}

static void wifi_event_handler(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)event_base;
    network_connectivity_t *connectivity = context;
    if (!is_started(connectivity)) {
        return;
    }
    if (event_id == WIFI_EVENT_STA_START) {
        set_state(connectivity, NETWORK_CONNECTIVITY_STATE_CONNECTING);
        if (esp_wifi_connect() != ESP_OK) {
            schedule_reconnect(connectivity);
        }
        return;
    }
    if (event_id != WIFI_EVENT_STA_DISCONNECTED) {
        return;
    }

    const wifi_event_sta_disconnected_t *disconnected = event_data;
    bool had_ip;
    portENTER_CRITICAL(&connectivity->lock);
    had_ip = connectivity->status.has_ip;
    connectivity->status.has_ip = false;
    connectivity->status.ipv4_address = 0U;
    connectivity->status.current_reconnect_delay_ms = 0U;
    connectivity->status.last_disconnect_reason =
        disconnected == NULL ? 0U : disconnected->reason;
    if (connectivity->status.disconnect_count < UINT32_MAX) {
        ++connectivity->status.disconnect_count;
    }
    portEXIT_CRITICAL(&connectivity->lock);
    if (had_ip) {
        publish_event(connectivity, NETWORK_CONNECTIVITY_EVENT_IP_LOST);
    }
    schedule_reconnect(connectivity);
}

static void ip_event_handler(
    void *context,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data)
{
    (void)event_base;
    network_connectivity_t *connectivity = context;
    if (!is_started(connectivity) || event_id != IP_EVENT_STA_GOT_IP || event_data == NULL) {
        return;
    }
    const ip_event_got_ip_t *got_ip = event_data;
    (void)esp_timer_stop(connectivity->reconnect_timer);
    bool state_changed;
    portENTER_CRITICAL(&connectivity->lock);
    state_changed = connectivity->status.state != NETWORK_CONNECTIVITY_STATE_READY;
    connectivity->status.has_ip = true;
    connectivity->status.ipv4_address = got_ip->ip_info.ip.addr;
    connectivity->status.ip_epoch = next_epoch(connectivity->status.ip_epoch);
    connectivity->status.reconnect_attempt = 0U;
    connectivity->status.current_reconnect_delay_ms = 0U;
    connectivity->status.state = NETWORK_CONNECTIVITY_STATE_READY;
    portEXIT_CRITICAL(&connectivity->lock);
    if (state_changed) {
        publish_event(connectivity, NETWORK_CONNECTIVITY_EVENT_STATE_CHANGED);
    }
    publish_event(connectivity, NETWORK_CONNECTIVITY_EVENT_IP_ACQUIRED);
}

network_connectivity_result_t network_connectivity_create(
    const network_connectivity_config_t *config,
    network_connectivity_t **connectivity)
{
    if (config == NULL || connectivity == NULL) {
        return NETWORK_CONNECTIVITY_RESULT_INVALID_ARGUMENT;
    }
    *connectivity = calloc(1U, sizeof(network_connectivity_t));
    if (*connectivity == NULL) {
        return NETWORK_CONNECTIVITY_RESULT_NO_MEMORY;
    }
    (*connectivity)->config = *config;
    (*connectivity)->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    (*connectivity)->status.state = NETWORK_CONNECTIVITY_STATE_STOPPED;
    const esp_timer_create_args_t timer_config = {
        .callback = reconnect_timer_callback,
        .arg = *connectivity,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "network_reconnect",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_config, &(*connectivity)->reconnect_timer) != ESP_OK) {
        free(*connectivity);
        *connectivity = NULL;
        return NETWORK_CONNECTIVITY_RESULT_PLATFORM_ERROR;
    }
    return NETWORK_CONNECTIVITY_RESULT_OK;
}

network_connectivity_result_t network_connectivity_start(
    network_connectivity_t *connectivity)
{
    if (connectivity == NULL) {
        return NETWORK_CONNECTIVITY_RESULT_INVALID_ARGUMENT;
    }
    portENTER_CRITICAL(&connectivity->lock);
    if (connectivity->started) {
        portEXIT_CRITICAL(&connectivity->lock);
        return NETWORK_CONNECTIVITY_RESULT_INVALID_STATE;
    }
    connectivity->started = true;
    connectivity->status.has_credentials = false;
    portEXIT_CRITICAL(&connectivity->lock);

    char ssid[NETWORK_SSID_CAPACITY] = {0};
    char password[NETWORK_PASSWORD_CAPACITY] = {0};
    const esp_err_t credential_result = load_credentials(ssid, password);
    if (credential_result == ESP_ERR_NVS_NOT_FOUND || credential_result == ESP_ERR_NOT_FOUND) {
        set_state(connectivity, NETWORK_CONNECTIVITY_STATE_UNCONFIGURED);
        return NETWORK_CONNECTIVITY_RESULT_OK;
    }
    if (credential_result != ESP_OK) {
        memset(password, 0, sizeof(password));
        mark_start_failed(connectivity);
        return NETWORK_CONNECTIVITY_RESULT_STORAGE_ERROR;
    }
    portENTER_CRITICAL(&connectivity->lock);
    connectivity->status.has_credentials = true;
    portEXIT_CRITICAL(&connectivity->lock);

    connectivity->station_netif = esp_netif_create_default_wifi_sta();
    if (connectivity->station_netif == NULL) {
        memset(password, 0, sizeof(password));
        mark_start_failed(connectivity);
        return NETWORK_CONNECTIVITY_RESULT_PLATFORM_ERROR;
    }
    const wifi_init_config_t wifi_initialization = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t result = esp_wifi_init(&wifi_initialization);
    if (result == ESP_OK) {
        connectivity->wifi_initialized = true;
        result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (result == ESP_OK) {
        result = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    wifi_config_t wifi_configuration = {0};
    if (result == ESP_OK) {
        const size_t ssid_length = strlen(ssid);
        const size_t password_length = strlen(password);
        memcpy(wifi_configuration.sta.ssid, ssid, ssid_length);
        memcpy(wifi_configuration.sta.password, password, password_length);
        wifi_configuration.sta.threshold.authmode =
            password_length == 0U ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        wifi_configuration.sta.pmf_cfg.capable = true;
        wifi_configuration.sta.pmf_cfg.required = false;
        result = esp_wifi_set_config(WIFI_IF_STA, &wifi_configuration);
    }
    memset(password, 0, sizeof(password));
    if (result == ESP_OK) {
        result = esp_event_handler_instance_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            wifi_event_handler,
            connectivity,
            &connectivity->wifi_event_instance);
    }
    if (result == ESP_OK) {
        result = esp_event_handler_instance_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            ip_event_handler,
            connectivity,
            &connectivity->ip_event_instance);
        if (result == ESP_OK) {
            connectivity->handlers_registered = true;
        } else {
            (void)esp_event_handler_instance_unregister(
                WIFI_EVENT, ESP_EVENT_ANY_ID, connectivity->wifi_event_instance);
        }
    }
    if (result == ESP_OK) {
        result = esp_wifi_start();
    }
    if (result != ESP_OK) {
        network_connectivity_stop(connectivity);
        mark_start_failed(connectivity);
        return NETWORK_CONNECTIVITY_RESULT_PLATFORM_ERROR;
    }
    return NETWORK_CONNECTIVITY_RESULT_OK;
}

void network_connectivity_stop(network_connectivity_t *connectivity)
{
    if (connectivity == NULL) {
        return;
    }
    bool had_ip;
    bool state_changed;
    portENTER_CRITICAL(&connectivity->lock);
    had_ip = connectivity->status.has_ip;
    state_changed = connectivity->status.state != NETWORK_CONNECTIVITY_STATE_STOPPED;
    connectivity->started = false;
    connectivity->status.has_ip = false;
    connectivity->status.ipv4_address = 0U;
    connectivity->status.current_reconnect_delay_ms = 0U;
    connectivity->status.reconnect_attempt = 0U;
    connectivity->status.state = NETWORK_CONNECTIVITY_STATE_STOPPED;
    portEXIT_CRITICAL(&connectivity->lock);
    (void)esp_timer_stop(connectivity->reconnect_timer);
    if (connectivity->handlers_registered) {
        (void)esp_event_handler_instance_unregister(
            WIFI_EVENT, ESP_EVENT_ANY_ID, connectivity->wifi_event_instance);
        (void)esp_event_handler_instance_unregister(
            IP_EVENT, IP_EVENT_STA_GOT_IP, connectivity->ip_event_instance);
        connectivity->handlers_registered = false;
    }
    if (connectivity->wifi_initialized) {
        (void)esp_wifi_disconnect();
        (void)esp_wifi_stop();
        (void)esp_wifi_deinit();
        connectivity->wifi_initialized = false;
    }
    if (connectivity->station_netif != NULL) {
        esp_netif_destroy_default_wifi(connectivity->station_netif);
        connectivity->station_netif = NULL;
    }
    if (had_ip) {
        publish_event(connectivity, NETWORK_CONNECTIVITY_EVENT_IP_LOST);
    }
    if (state_changed) {
        publish_event(connectivity, NETWORK_CONNECTIVITY_EVENT_STATE_CHANGED);
    }
}

void network_connectivity_destroy(network_connectivity_t *connectivity)
{
    if (connectivity == NULL) {
        return;
    }
    network_connectivity_stop(connectivity);
    if (connectivity->reconnect_timer != NULL) {
        (void)esp_timer_delete(connectivity->reconnect_timer);
    }
    free(connectivity);
}

void network_connectivity_get_status(
    network_connectivity_t *connectivity,
    network_connectivity_status_t *status)
{
    if (connectivity == NULL || status == NULL) {
        return;
    }
    portENTER_CRITICAL(&connectivity->lock);
    *status = connectivity->status;
    portEXIT_CRITICAL(&connectivity->lock);
}
