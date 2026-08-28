#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *const TAG = "terminal_lifecycle";

static void terminal_storage_initialize(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES || result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

void app_main(void)
{
    terminal_storage_initialize();
    ESP_LOGI(TAG, "event=terminal_boot status=storage_ready");
}
