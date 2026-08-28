#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2s_std.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wn_models.h"
#include "model_path.h"

#include "board_config.h"
#include "wakenet_listener.h"

#define WAKENET_SAMPLE_RATE_HZ 16000
#define WAKENET_I2S_CHANNELS 2
#define WAKENET_STARTUP_READS 4
#define WAKENET_FETCH_TIMEOUT_MS 100

static const char *TAG = "wakenet_listener";

static srmodel_list_t *models;
static const esp_afe_sr_iface_t *afe_handle;
static esp_afe_sr_data_t *afe_data;
static i2s_chan_handle_t rx_channel;
static int16_t *feed_samples;
static int32_t *i2s_words;
static int feed_chunk_samples;
static int feed_channel_count;
static unsigned long wake_count;
static bool initialized;
static bool listening;

static i2s_std_config_t make_rx_config(void)
{
    const i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(WAKENET_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws = BOARD_I2S_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = BOARD_I2S_RX_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    return config;
}

static esp_err_t start_i2s(void)
{
    if (rx_channel != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    const i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    esp_err_t result = i2s_new_channel(&channel_config, NULL, &rx_channel);
    if (result != ESP_OK) {
        rx_channel = NULL;
        return result;
    }

    i2s_std_config_t std_config = make_rx_config();
    result = i2s_channel_init_std_mode(rx_channel, &std_config);
    if (result == ESP_OK) {
        result = i2s_channel_enable(rx_channel);
    }
    if (result != ESP_OK) {
        (void)i2s_del_channel(rx_channel);
        rx_channel = NULL;
        return result;
    }

    for (int read = 0; read < WAKENET_STARTUP_READS; ++read) {
        size_t bytes_read = 0;
        result = i2s_channel_read(
            rx_channel,
            i2s_words,
            (size_t)feed_chunk_samples * WAKENET_I2S_CHANNELS * sizeof(i2s_words[0]),
            &bytes_read,
            pdMS_TO_TICKS(1000));
        if (result != ESP_OK) {
            return result;
        }
    }
    return ESP_OK;
}

static esp_err_t stop_i2s(void)
{
    if (rx_channel == NULL) {
        return ESP_OK;
    }
    esp_err_t result = i2s_channel_disable(rx_channel);
    const esp_err_t delete_result = i2s_del_channel(rx_channel);
    rx_channel = NULL;
    return result != ESP_OK ? result : delete_result;
}

static esp_err_t read_feed_chunk(void)
{
    const size_t expected_words =
        (size_t)feed_chunk_samples * WAKENET_I2S_CHANNELS;
    size_t total_words = 0;

    while (total_words < expected_words) {
        size_t bytes_read = 0;
        const esp_err_t result = i2s_channel_read(
            rx_channel,
            &i2s_words[total_words],
            (expected_words - total_words) * sizeof(i2s_words[0]),
            &bytes_read,
            pdMS_TO_TICKS(1000));
        if (result != ESP_OK) {
            return result;
        }
        if (bytes_read == 0 || bytes_read % sizeof(i2s_words[0]) != 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        total_words += bytes_read / sizeof(i2s_words[0]);
    }

    for (int sample = 0; sample < feed_chunk_samples; ++sample) {
        /* INMP441 L/R is tied to GND, so valid audio is in the left slot. */
        feed_samples[sample] =
            (int16_t)(i2s_words[(size_t)sample * WAKENET_I2S_CHANNELS] >> 16);
    }
    return ESP_OK;
}

esp_err_t wakenet_listener_init(void)
{
    if (initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    models = esp_srmodel_init("model");
    if (models == NULL) {
        ESP_LOGE(TAG, "Unable to load models from partition 'model'");
        return ESP_ERR_NOT_FOUND;
    }

    char *model_name = esp_srmodel_filter(models, ESP_WN_PREFIX, NULL);
    if (model_name == NULL) {
        ESP_LOGE(TAG, "No WakeNet model found in model partition");
        return ESP_ERR_NOT_FOUND;
    }

    afe_config_t *config =
        afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (config == NULL || config->wakenet_model_name == NULL) {
        ESP_LOGE(TAG, "Unable to create a single-microphone WakeNet config");
        if (config != NULL) {
            afe_config_free(config);
        }
        return ESP_ERR_NOT_SUPPORTED;
    }

    config->aec_init = false;
    config->se_init = false;
    config->ns_init = false;
    config->vad_init = false;
    config->wakenet_init = true;
    config->wakenet_mode = DET_MODE_90;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;

    afe_handle = esp_afe_handle_from_config(config);
    if (afe_handle != NULL) {
        afe_data = afe_handle->create_from_config(config);
    }
    afe_config_free(config);
    if (afe_handle == NULL || afe_data == NULL) {
        ESP_LOGE(TAG, "AFE instance creation failed");
        return ESP_ERR_NO_MEM;
    }

    feed_chunk_samples = afe_handle->get_feed_chunksize(afe_data);
    feed_channel_count = afe_handle->get_feed_channel_num(afe_data);
    const int sample_rate = afe_handle->get_samp_rate(afe_data);
    if (feed_chunk_samples <= 0 || feed_channel_count != 1 ||
        sample_rate != WAKENET_SAMPLE_RATE_HZ) {
        ESP_LOGE(TAG, "Unexpected AFE format: rate=%d chunk=%d channels=%d",
                 sample_rate, feed_chunk_samples, feed_channel_count);
        return ESP_ERR_INVALID_RESPONSE;
    }

    feed_samples = heap_caps_malloc(
        (size_t)feed_chunk_samples * sizeof(feed_samples[0]),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    i2s_words = heap_caps_malloc(
        (size_t)feed_chunk_samples * WAKENET_I2S_CHANNELS * sizeof(i2s_words[0]),
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (feed_samples == NULL || i2s_words == NULL) {
        ESP_LOGE(TAG, "Unable to allocate WakeNet feed buffers");
        return ESP_ERR_NO_MEM;
    }

    initialized = true;
    const esp_err_t result = wakenet_listener_resume();
    if (result != ESP_OK) {
        return result;
    }

    ESP_LOGI(TAG,
             "READY: model=%s phrase=你好小智 rate=%d chunk=%d channels=%d",
             model_name, sample_rate, feed_chunk_samples, feed_channel_count);
    afe_handle->print_pipeline(afe_data);
    return ESP_OK;
}

esp_err_t wakenet_listener_poll(bool *detected)
{
    if (!initialized || !listening || detected == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    *detected = false;

    esp_err_t result = read_feed_chunk();
    if (result != ESP_OK) {
        return result;
    }
    if (afe_handle->feed(afe_data, feed_samples) < 0) {
        return ESP_FAIL;
    }

    afe_fetch_result_t *fetch = afe_handle->fetch_with_delay(
        afe_data, pdMS_TO_TICKS(WAKENET_FETCH_TIMEOUT_MS));
    if (fetch == NULL) {
        return ESP_OK;
    }
    if (fetch->ret_value == ESP_FAIL) {
        return ESP_FAIL;
    }
    if (fetch->wakeup_state == WAKENET_DETECTED) {
        ++wake_count;
        *detected = true;
        ESP_LOGI(TAG,
                 "DETECTED: count=%lu model_index=%d word_index=%d volume=%.1f dB",
                 wake_count,
                 fetch->wakenet_model_index,
                 fetch->wake_word_index,
                 (double)fetch->data_volume);
    }
    return ESP_OK;
}

esp_err_t wakenet_listener_pause(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!listening) {
        return ESP_OK;
    }
    listening = false;
    if (afe_handle->disable_wakenet(afe_data) < 0) {
        return ESP_FAIL;
    }
    return stop_i2s();
}

esp_err_t wakenet_listener_resume(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (listening) {
        return ESP_OK;
    }
    if (afe_handle->reset_buffer(afe_data) < 0 ||
        afe_handle->enable_wakenet(afe_data) < 0) {
        return ESP_FAIL;
    }
    const esp_err_t result = start_i2s();
    if (result == ESP_OK) {
        listening = true;
    }
    return result;
}

void wakenet_listener_get_status(wakenet_listener_status_t *status)
{
    if (status == NULL) {
        return;
    }
    status->sample_rate_hz = initialized ? afe_handle->get_samp_rate(afe_data) : 0;
    status->feed_samples = feed_chunk_samples;
    status->feed_channels = feed_channel_count;
    status->wake_count = wake_count;
}
