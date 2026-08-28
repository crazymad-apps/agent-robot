#include "wifi_station.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASSWORD_KEY "password"

static const char *TAG = "s3_wifi";
static EventGroupHandle_t connection_events;

static esp_err_t load_credentials(char *ssid,
                                  size_t ssid_capacity,
                                  char *password,
                                  size_t password_capacity)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result != ESP_OK) {
        return result;
    }
    size_t ssid_length = ssid_capacity;
    result = nvs_get_str(handle, WIFI_NVS_SSID_KEY, ssid, &ssid_length);
    if (result == ESP_OK) {
        size_t password_length = password_capacity;
        result = nvs_get_str(handle, WIFI_NVS_PASSWORD_KEY, password, &password_length);
    }
    nvs_close(handle);
    return result;
}

static void wifi_event_handler(void *argument,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)argument;
    (void)event_base;
    (void)event_data;
    if (event_id == WIFI_EVENT_STA_START || event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(connection_events, WIFI_CONNECTED_BIT);
        const esp_err_t result = esp_wifi_connect();
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi connect request failed: %s", esp_err_to_name(result));
        }
    }
}

static void ip_event_handler(void *argument,
                             esp_event_base_t event_base,
                             int32_t event_id,
                             void *event_data)
{
    (void)argument;
    (void)event_base;
    if (event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }
    const ip_event_got_ip_t *event = event_data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(connection_events, WIFI_CONNECTED_BIT);
    ESP_LOGI(TAG, "Wi-Fi connected: ip=%s", ip);
}

esp_err_t wifi_station_init(void)
{
    connection_events = xEventGroupCreate();
    if (connection_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) {
        return result;
    }

    char ssid[33] = {0};
    char password[64] = {0};
    result = load_credentials(ssid, sizeof(ssid), password, sizeof(password));
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "S2 Wi-Fi credentials not found in NVS namespace '%s': %s",
                 WIFI_NVS_NAMESPACE,
                 esp_err_to_name(result));
        return result;
    }

    if ((result = esp_netif_init()) != ESP_OK ||
        (result = esp_event_loop_create_default()) != ESP_OK) {
        memset(password, 0, sizeof(password));
        return result;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        memset(password, 0, sizeof(password));
        return ESP_ERR_NO_MEM;
    }

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    if ((result = esp_wifi_init(&init_config)) != ESP_OK ||
        (result = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (result = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
        (result = esp_event_handler_register(
             WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL)) != ESP_OK ||
        (result = esp_event_handler_register(
             IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_handler, NULL)) != ESP_OK) {
        memset(password, 0, sizeof(password));
        return result;
    }

    wifi_config_t config = {0};
    const size_t ssid_length = strlen(ssid);
    const size_t password_length = strlen(password);
    memcpy(config.sta.ssid, ssid, ssid_length);
    memcpy(config.sta.password, password, password_length);
    config.sta.threshold.authmode = password_length == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;
    result = esp_wifi_set_config(WIFI_IF_STA, &config);
    memset(password, 0, sizeof(password));
    if (result != ESP_OK) {
        return result;
    }

    esp_log_level_set("wifi", ESP_LOG_WARN);
    return esp_wifi_start();
}

esp_err_t wifi_station_wait_connected(TickType_t timeout)
{
    if (connection_events == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const EventBits_t bits = xEventGroupWaitBits(
        connection_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, timeout);
    return (bits & WIFI_CONNECTED_BIT) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}
