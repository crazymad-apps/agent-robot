#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "sdkconfig.h"

#include "board_config.h"
#include "voice_demo_client.h"
#include "wifi_station.h"

#define SAMPLE_RATE_HZ 16000
#define MAX_RECORD_SECONDS 10
#define MAX_RECORD_SAMPLES (SAMPLE_RATE_HZ * MAX_RECORD_SECONDS)
#define RX_DMA_FRAMES 256
#define I2S_WORDS_PER_FRAME 2
#define RX_STARTUP_READS 8
#define TX_FRAME_MS 20
#define TX_SAMPLES_PER_FRAME ((SAMPLE_RATE_HZ * TX_FRAME_MS) / 1000)
#define TX_WORDS_PER_FRAME (TX_SAMPLES_PER_FRAME * I2S_WORDS_PER_FRAME)
#define PLAYBACK_VOLUME_PERCENT 75
#define BUTTON_DEBOUNCE_MS 30
#define MIN_RECORD_SAMPLES (SAMPLE_RATE_HZ / 5)
#define RESPONSE_TIMEOUT_MS 20000

typedef enum {
    STATUS_CONNECTING,
    STATUS_IDLE,
    STATUS_LISTENING,
    STATUS_PROCESSING,
    STATUS_PLAYING,
    STATUS_ERROR,
} demo_status_t;

static const char *TAG = "agent_robot_s3";
static int32_t rx_words[RX_DMA_FRAMES * I2S_WORDS_PER_FRAME];
static int32_t tx_words[TX_WORDS_PER_FRAME];

static i2s_std_config_t make_i2s_config(void)
{
    const i2s_std_config_t config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws = BOARD_I2S_WS_GPIO,
            .dout = BOARD_I2S_TX_GPIO,
            .din = BOARD_I2S_RX_GPIO,
        },
    };
    return config;
}

static esp_err_t create_rgb(led_strip_handle_t *strip)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_ONBOARD_RGB_GPIO,
        .max_leds = BOARD_ONBOARD_RGB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    return led_strip_new_rmt_device(&strip_config, &rmt_config, strip);
}

static void set_status(led_strip_handle_t strip, demo_status_t status)
{
    uint8_t red = 0;
    uint8_t green = 0;
    uint8_t blue = 0;
    switch (status) {
    case STATUS_CONNECTING:
        blue = 8;
        break;
    case STATUS_IDLE:
        blue = 24;
        break;
    case STATUS_LISTENING:
        green = 24;
        break;
    case STATUS_PROCESSING:
        red = 10;
        blue = 24;
        break;
    case STATUS_PLAYING:
        red = 18;
        green = 8;
        break;
    case STATUS_ERROR:
        red = 24;
        break;
    }
    ESP_ERROR_CHECK(led_strip_set_pixel(strip, 0, red, green, blue));
    ESP_ERROR_CHECK(led_strip_refresh(strip));
}

static void init_gpio(void)
{
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << BOARD_PTT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
    ESP_ERROR_CHECK(gpio_reset_pin(BOARD_AMP_ENABLE_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(BOARD_AMP_ENABLE_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 0));
}

static void wait_for_button_press(void)
{
    while (gpio_get_level(BOARD_PTT_BUTTON_GPIO) != 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS));
}

static size_t record_while_pressed(int16_t *samples)
{
    i2s_chan_handle_t rx = NULL;
    const i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, NULL, &rx));
    i2s_std_config_t config = make_i2s_config();
    config.gpio_cfg.dout = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &config));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));

    for (int index = 0; index < RX_STARTUP_READS; ++index) {
        size_t bytes_read = 0;
        ESP_ERROR_CHECK(i2s_channel_read(
            rx, rx_words, sizeof(rx_words), &bytes_read, pdMS_TO_TICKS(1000)));
    }

    size_t sample_count = 0;
    while (sample_count < MAX_RECORD_SAMPLES) {
        size_t bytes_read = 0;
        ESP_ERROR_CHECK(i2s_channel_read(
            rx, rx_words, sizeof(rx_words), &bytes_read, pdMS_TO_TICKS(1000)));
        const size_t words_read = bytes_read / sizeof(rx_words[0]);
        for (size_t word = 0;
             word + 1 < words_read && sample_count < MAX_RECORD_SAMPLES;
             word += I2S_WORDS_PER_FRAME) {
            samples[sample_count++] = (int16_t)(rx_words[word] >> 16);
        }
        if (sample_count >= MIN_RECORD_SAMPLES &&
            gpio_get_level(BOARD_PTT_BUTTON_GPIO) != 0) {
            break;
        }
    }

    ESP_ERROR_CHECK(i2s_channel_disable(rx));
    ESP_ERROR_CHECK(i2s_del_channel(rx));
    return sample_count;
}

static void play_pcm(const int16_t *samples, size_t sample_count)
{
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, &tx, NULL));
    i2s_std_config_t config = make_i2s_config();
    config.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &config));
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 1));

    for (size_t offset = 0; offset < sample_count; offset += TX_SAMPLES_PER_FRAME) {
        for (size_t frame_sample = 0; frame_sample < TX_SAMPLES_PER_FRAME; ++frame_sample) {
            const size_t index = offset + frame_sample;
            const int32_t scaled_sample =
                index < sample_count
                    ? ((int32_t)samples[index] * PLAYBACK_VOLUME_PERCENT) / 100
                    : 0;
            const int32_t output = scaled_sample << 16;
            tx_words[frame_sample * 2] = output;
            tx_words[frame_sample * 2 + 1] = output;
        }
        size_t bytes_written = 0;
        ESP_ERROR_CHECK(i2s_channel_write(
            tx, tx_words, sizeof(tx_words), &bytes_written, pdMS_TO_TICKS(1000)));
        ESP_ERROR_CHECK(bytes_written == sizeof(tx_words) ? ESP_OK : ESP_ERR_INVALID_SIZE);
    }

    memset(tx_words, 0, sizeof(tx_words));
    size_t bytes_written = 0;
    ESP_ERROR_CHECK(i2s_channel_write(
        tx, tx_words, sizeof(tx_words), &bytes_written, pdMS_TO_TICKS(1000)));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 0));
    ESP_ERROR_CHECK(i2s_channel_disable(tx));
    ESP_ERROR_CHECK(i2s_del_channel(tx));
}

void app_main(void)
{
    led_strip_handle_t rgb = NULL;
    ESP_LOGI(TAG, "Version=%s board=%s", DEMO_VERSION, BOARD_NAME);
    init_gpio();
    ESP_ERROR_CHECK(create_rgb(&rgb));
    set_status(rgb, STATUS_CONNECTING);

    int16_t *capture = heap_caps_malloc(
        MAX_RECORD_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *playback = heap_caps_malloc(
        MAX_RECORD_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (capture == NULL || playback == NULL) {
        set_status(rgb, STATUS_ERROR);
        ESP_LOGE(TAG, "PSRAM allocation failed");
        return;
    }
    if (CONFIG_VOICE_DEMO_WS_URI[0] == '\0') {
        set_status(rgb, STATUS_ERROR);
        ESP_LOGE(TAG, "VOICE_DEMO_WS_URI is empty; configure it with menuconfig");
        return;
    }

    ESP_ERROR_CHECK(wifi_station_init());
    ESP_ERROR_CHECK(wifi_station_wait_connected(pdMS_TO_TICKS(30000)));
    ESP_ERROR_CHECK(voice_demo_client_init(
        CONFIG_VOICE_DEMO_WS_URI,
        (uint8_t *)playback,
        MAX_RECORD_SAMPLES * sizeof(playback[0])));

    while (true) {
        if (!voice_demo_client_is_connected()) {
            set_status(rgb, STATUS_CONNECTING);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        set_status(rgb, STATUS_IDLE);
        wait_for_button_press();
        if (!voice_demo_client_is_connected()) {
            continue;
        }

        set_status(rgb, STATUS_LISTENING);
        const size_t capture_count = record_while_pressed(capture);
        ESP_LOGI(TAG, "Captured %u samples", (unsigned int)capture_count);

        set_status(rgb, STATUS_PROCESSING);
        size_t playback_count = 0;
        const esp_err_t result = voice_demo_client_roundtrip(
            capture,
            capture_count,
            &playback_count,
            pdMS_TO_TICKS(RESPONSE_TIMEOUT_MS));
        if (result != ESP_OK) {
            set_status(rgb, STATUS_ERROR);
            ESP_LOGE(TAG, "PCM roundtrip failed: %s", esp_err_to_name(result));
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        set_status(rgb, STATUS_PLAYING);
        play_pcm(playback, playback_count);
        ESP_LOGI(TAG, "Playback complete: samples=%u", (unsigned int)playback_count);
    }
}
