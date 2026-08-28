#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "status_display.h"
#include "wifi_manager.h"

#define WIFI_NVS_NAMESPACE "wifi_cfg"
#define WIFI_NVS_SSID_KEY "ssid"
#define WIFI_NVS_PASSWORD_KEY "password"
#define WIFI_CONSOLE_STACK_SIZE 4096
#define WIFI_CONSOLE_PRIORITY 2
#define WIFI_MAX_RECONNECT_DELAY_SECONDS 30

static const char *TAG = "wifi_manager";

static esp_timer_handle_t reconnect_timer;
static esp_event_handler_instance_t wifi_event_instance;
static esp_event_handler_instance_t ip_event_instance;
static bool wifi_started;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static wifi_manager_status_t current_status = {
    .state = WIFI_MANAGER_UNCONFIGURED,
    .rssi = -127,
};

static esp_err_t initialize_console_input(void)
{
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    fflush(stdout);
    (void)fsync(fileno(stdout));
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);
    (void)fcntl(fileno(stdout), F_SETFL, 0);
    (void)fcntl(fileno(stdin), F_SETFL, 0);

    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 256,
        .rx_buffer_size = 256,
    };
    const esp_err_t result = usb_serial_jtag_driver_install(&config);
    if (result != ESP_OK) {
        return result;
    }
    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdin, NULL, _IONBF, 0);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static uint32_t ssid_identifier(const char *ssid)
{
    uint32_t hash = 2166136261u;
    for (const unsigned char *cursor = (const unsigned char *)ssid; *cursor != 0; ++cursor) {
        hash ^= *cursor;
        hash *= 16777619u;
    }
    return hash & 0xFFFFu;
}

static robot_network_status_t to_display_status(wifi_manager_state_t state)
{
    switch (state) {
    case WIFI_MANAGER_UNCONFIGURED:
        return ROBOT_NETWORK_SETUP;
    case WIFI_MANAGER_CONNECTING:
        return ROBOT_NETWORK_CONNECTING;
    case WIFI_MANAGER_CONNECTED:
        return ROBOT_NETWORK_CONNECTED;
    case WIFI_MANAGER_DISCONNECTED:
        return ROBOT_NETWORK_DISCONNECTED;
    case WIFI_MANAGER_ERROR:
    default:
        return ROBOT_NETWORK_ERROR;
    }
}

static void set_state(wifi_manager_state_t state)
{
    portENTER_CRITICAL(&status_lock);
    current_status.state = state;
    portEXIT_CRITICAL(&status_lock);
    status_display_set_network(to_display_status(state));
}

static bool credentials_available(void)
{
    bool available;
    portENTER_CRITICAL(&status_lock);
    available = current_status.has_credentials;
    portEXIT_CRITICAL(&status_lock);
    return available;
}

static esp_err_t load_credentials(char *ssid, size_t ssid_size,
                                  char *password, size_t password_size)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (result != ESP_OK) {
        return result;
    }

    size_t stored_ssid_size = ssid_size;
    result = nvs_get_str(handle, WIFI_NVS_SSID_KEY, ssid, &stored_ssid_size);
    if (result == ESP_OK) {
        size_t stored_password_size = password_size;
        result = nvs_get_str(handle, WIFI_NVS_PASSWORD_KEY, password, &stored_password_size);
    }
    nvs_close(handle);
    return result;
}

static esp_err_t save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t result = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result != ESP_OK) {
        return result;
    }
    result = nvs_set_str(handle, WIFI_NVS_SSID_KEY, ssid);
    if (result == ESP_OK) {
        result = nvs_set_str(handle, WIFI_NVS_PASSWORD_KEY, password);
    }
    if (result == ESP_OK) {
        result = nvs_commit(handle);
    }
    nvs_close(handle);
    return result;
}

static esp_err_t clear_credentials(void)
{
    portENTER_CRITICAL(&status_lock);
    current_status.has_credentials = false;
    current_status.reconnect_count = 0;
    current_status.last_disconnect_reason = 0;
    current_status.rssi = -127;
    current_status.ip[0] = '\0';
    portEXIT_CRITICAL(&status_lock);

    if (reconnect_timer != NULL) {
        (void)esp_timer_stop(reconnect_timer);
    }
    if (wifi_started) {
        (void)esp_wifi_disconnect();
    }

    nvs_handle_t handle;
    esp_err_t result = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        result = nvs_erase_all(handle);
        if (result == ESP_OK) {
            result = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    set_state(result == ESP_OK ? WIFI_MANAGER_UNCONFIGURED : WIFI_MANAGER_ERROR);
    return result;
}

static esp_err_t apply_credentials(const char *ssid, const char *password, bool persist)
{
    const size_t ssid_length = strlen(ssid);
    const size_t password_length = strlen(password);
    if (ssid_length == 0 || ssid_length > 32 || password_length > 63 ||
        (password_length > 0 && password_length < 8)) {
        return ESP_ERR_INVALID_ARG;
    }

    if (persist) {
        esp_err_t result = save_credentials(ssid, password);
        if (result != ESP_OK) {
            return result;
        }
    }

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, ssid_length);
    memcpy(config.sta.password, password, password_length);
    config.sta.threshold.authmode = password_length == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    if (reconnect_timer != NULL) {
        (void)esp_timer_stop(reconnect_timer);
    }
    portENTER_CRITICAL(&status_lock);
    current_status.has_credentials = true;
    current_status.reconnect_count = 0;
    current_status.last_disconnect_reason = 0;
    current_status.rssi = -127;
    current_status.ip[0] = '\0';
    portEXIT_CRITICAL(&status_lock);

    if (wifi_started) {
        (void)esp_wifi_disconnect();
    }
    esp_err_t result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result == ESP_OK && wifi_started) {
        set_state(WIFI_MANAGER_CONNECTING);
        result = esp_wifi_connect();
    }
    if (result != ESP_OK) {
        set_state(WIFI_MANAGER_ERROR);
        return result;
    }

    ESP_LOGI(TAG, "Credentials accepted for ssid_id=%04" PRIx32, ssid_identifier(ssid));
    return ESP_OK;
}

static void reconnect_timer_callback(void *argument)
{
    (void)argument;
    if (!credentials_available()) {
        return;
    }
    set_state(WIFI_MANAGER_CONNECTING);
    const esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Reconnect start failed: %s", esp_err_to_name(result));
    }
}

static void schedule_reconnect(void)
{
    if (!credentials_available()) {
        set_state(WIFI_MANAGER_UNCONFIGURED);
        return;
    }

    uint32_t attempt;
    portENTER_CRITICAL(&status_lock);
    attempt = ++current_status.reconnect_count;
    portEXIT_CRITICAL(&status_lock);

    uint32_t delay_seconds = 1u << (attempt > 5 ? 5 : attempt - 1);
    if (delay_seconds > WIFI_MAX_RECONNECT_DELAY_SECONDS) {
        delay_seconds = WIFI_MAX_RECONNECT_DELAY_SECONDS;
    }
    set_state(WIFI_MANAGER_DISCONNECTED);
    (void)esp_timer_stop(reconnect_timer);
    const esp_err_t result = esp_timer_start_once(reconnect_timer, delay_seconds * 1000000ULL);
    if (result == ESP_OK) {
        ESP_LOGW(TAG, "Reconnect attempt=%" PRIu32 " in %" PRIu32 "s",
                 attempt, delay_seconds);
    } else {
        ESP_LOGE(TAG, "Reconnect timer failed: %s", esp_err_to_name(result));
        set_state(WIFI_MANAGER_ERROR);
    }
}

static void handle_wifi_event(void *argument, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_base;
    if (event_id == WIFI_EVENT_STA_START) {
        if (credentials_available()) {
            set_state(WIFI_MANAGER_CONNECTING);
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_connect());
        } else {
            set_state(WIFI_MANAGER_UNCONFIGURED);
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;
        portENTER_CRITICAL(&status_lock);
        current_status.last_disconnect_reason = event == NULL ? 0 : event->reason;
        current_status.rssi = -127;
        current_status.ip[0] = '\0';
        portEXIT_CRITICAL(&status_lock);
        schedule_reconnect();
    }
}

static void handle_ip_event(void *argument, esp_event_base_t event_base,
                            int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_base;
    if (event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const ip_event_got_ip_t *event = event_data;
    char ip[16];
    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&event->ip_info.ip));
    wifi_ap_record_t access_point = {0};
    const int8_t rssi = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK ? access_point.rssi : -127;

    (void)esp_timer_stop(reconnect_timer);
    portENTER_CRITICAL(&status_lock);
    current_status.state = WIFI_MANAGER_CONNECTED;
    current_status.reconnect_count = 0;
    current_status.last_disconnect_reason = 0;
    current_status.rssi = rssi;
    snprintf(current_status.ip, sizeof(current_status.ip), "%s", ip);
    portEXIT_CRITICAL(&status_lock);
    status_display_set_network(ROBOT_NETWORK_CONNECTED);
    ESP_LOGI(TAG, "Connected: ip=%s rssi=%d dBm", ip, rssi);
}

static bool read_console_line(char *buffer, size_t buffer_size)
{
    if (fgets(buffer, buffer_size, stdin) == NULL) {
        return false;
    }
    const size_t length = strcspn(buffer, "\r\n");
    const bool complete_line = buffer[length] != '\0' || length < buffer_size - 1;
    buffer[length] = '\0';
    if (!complete_line) {
        int character;
        while ((character = getchar()) != '\n' && character != '\r' && character != EOF) {
        }
        return false;
    }
    return true;
}

static const char *state_name(wifi_manager_state_t state)
{
    switch (state) {
    case WIFI_MANAGER_UNCONFIGURED:
        return "unconfigured";
    case WIFI_MANAGER_CONNECTING:
        return "connecting";
    case WIFI_MANAGER_CONNECTED:
        return "connected";
    case WIFI_MANAGER_DISCONNECTED:
        return "disconnected";
    case WIFI_MANAGER_ERROR:
    default:
        return "error";
    }
}

static void print_status(void)
{
    wifi_manager_status_t status;
    wifi_manager_get_status(&status);
    printf("wifi state=%s configured=%s ip=%s rssi=%d reconnects=%" PRIu32
           " last_reason=%u\n",
           state_name(status.state), status.has_credentials ? "yes" : "no",
           status.ip[0] == '\0' ? "-" : status.ip, status.rssi,
           status.reconnect_count, status.last_disconnect_reason);
}

static void console_task(void *argument)
{
    (void)argument;
    char command[24];
    char ssid[33];
    char password[64];

    printf("Wi-Fi console ready: wifi set | wifi status | wifi clear\n");
    while (true) {
        if (!read_console_line(command, sizeof(command))) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (strcmp(command, "wifi set") == 0) {
            printf("SSID> ");
            fflush(stdout);
            if (!read_console_line(ssid, sizeof(ssid))) {
                printf("SSID invalid or too long\n");
                continue;
            }
            printf("PASSWORD> ");
            fflush(stdout);
            if (!read_console_line(password, sizeof(password))) {
                printf("Password invalid or too long\n");
                memset(password, 0, sizeof(password));
                continue;
            }
            const esp_err_t result = apply_credentials(ssid, password, true);
            memset(password, 0, sizeof(password));
            if (result == ESP_OK) {
                printf("Credentials saved; connecting\n");
            } else {
                printf("Credentials rejected: %s\n", esp_err_to_name(result));
            }
        } else if (strcmp(command, "wifi status") == 0) {
            print_status();
        } else if (strcmp(command, "wifi clear") == 0) {
            const esp_err_t result = clear_credentials();
            if (result == ESP_OK) {
                printf("Wi-Fi credentials cleared\n");
            } else {
                printf("Clear failed: %s\n", esp_err_to_name(result));
            }
        } else if (command[0] != '\0') {
            printf("Unknown command. Use: wifi set | wifi status | wifi clear\n");
        }
    }
}

esp_err_t wifi_manager_init(void)
{
    esp_err_t result = initialize_console_input();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Interactive USB console init failed: %s", esp_err_to_name(result));
        set_state(WIFI_MANAGER_ERROR);
        return result;
    }

    /* ESP-IDF's Wi-Fi INFO log includes the AP SSID/BSSID.  Keep normal
     * diagnostics on this module's privacy-preserving status lines instead. */
    esp_log_level_set("wifi", ESP_LOG_WARN);

    result = nvs_flash_init();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed without erasing data: %s", esp_err_to_name(result));
        set_state(WIFI_MANAGER_ERROR);
        return result;
    }
    if ((result = esp_netif_init()) != ESP_OK ||
        (result = esp_event_loop_create_default()) != ESP_OK) {
        set_state(WIFI_MANAGER_ERROR);
        return result;
    }
    if (esp_netif_create_default_wifi_sta() == NULL) {
        set_state(WIFI_MANAGER_ERROR);
        return ESP_ERR_NO_MEM;
    }

    const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    if ((result = esp_wifi_init(&init_config)) != ESP_OK ||
        (result = esp_wifi_set_storage(WIFI_STORAGE_RAM)) != ESP_OK ||
        (result = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) {
        set_state(WIFI_MANAGER_ERROR);
        return result;
    }

    const esp_timer_create_args_t timer_config = {
        .callback = reconnect_timer_callback,
        .name = "wifi_retry",
    };
    if ((result = esp_timer_create(&timer_config, &reconnect_timer)) != ESP_OK ||
        (result = esp_event_handler_instance_register(
             WIFI_EVENT, ESP_EVENT_ANY_ID, handle_wifi_event, NULL, &wifi_event_instance)) != ESP_OK ||
        (result = esp_event_handler_instance_register(
             IP_EVENT, IP_EVENT_STA_GOT_IP, handle_ip_event, NULL, &ip_event_instance)) != ESP_OK) {
        set_state(WIFI_MANAGER_ERROR);
        return result;
    }

    char ssid[33] = {0};
    char password[64] = {0};
    result = load_credentials(ssid, sizeof(ssid), password, sizeof(password));
    if (result == ESP_OK) {
        result = apply_credentials(ssid, password, false);
    } else if (result == ESP_ERR_NOT_FOUND || result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
        set_state(WIFI_MANAGER_UNCONFIGURED);
        ESP_LOGI(TAG, "No Wi-Fi credentials; use serial command 'wifi set'");
    }
    memset(password, 0, sizeof(password));
    if (result != ESP_OK) {
        set_state(WIFI_MANAGER_ERROR);
        return result;
    }

    result = esp_wifi_start();
    if (result != ESP_OK) {
        set_state(WIFI_MANAGER_ERROR);
        return result;
    }
    wifi_started = true;
    return xTaskCreate(console_task, "wifi_console", WIFI_CONSOLE_STACK_SIZE,
                       NULL, WIFI_CONSOLE_PRIORITY, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

void wifi_manager_get_status(wifi_manager_status_t *status)
{
    if (status == NULL) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    *status = current_status;
    portEXIT_CRITICAL(&status_lock);
}
