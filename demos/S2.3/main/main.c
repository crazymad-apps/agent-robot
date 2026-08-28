#include <inttypes.h>
#include <limits.h>
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

#include "board_config.h"
#include "status_display.h"
#include "wakenet_listener.h"
#include "wifi_manager.h"

#define SAMPLE_RATE_HZ 16000
#define RECORD_SECONDS 5
#define RECORD_SAMPLES (SAMPLE_RATE_HZ * RECORD_SECONDS)
#define RX_DMA_FRAMES 256
#define I2S_WORDS_PER_FRAME 2
#define RX_STARTUP_READS 12
#define TX_FRAME_MS 20
#define TX_SAMPLES_PER_FRAME ((SAMPLE_RATE_HZ * TX_FRAME_MS) / 1000)
#define TX_WORDS_PER_FRAME (TX_SAMPLES_PER_FRAME * I2S_WORDS_PER_FRAME)
#define PLAYBACK_GAIN 4
#define BUTTON_POLL_MS 10
#define BUTTON_DEBOUNCE_POLLS 3

static const char *TAG = "agent_robot_loopback";

static const int16_t SINE_TABLE[] = {
    0, 196, 362, 473, 512, 473, 362, 196,
    0, -196, -362, -473, -512, -473, -362, -196,
};

static int32_t rx_words[RX_DMA_FRAMES * I2S_WORDS_PER_FRAME];
static int32_t tx_words[TX_WORDS_PER_FRAME];

static esp_err_t create_onboard_rgb(led_strip_handle_t *strip)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_ONBOARD_RGB_GPIO,
        .max_leds = BOARD_ONBOARD_RGB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    return led_strip_new_rmt_device(&strip_config, &rmt_config, strip);
}

static void set_rgb(led_strip_handle_t strip, uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(strip, 0, red, green, blue));
    ESP_ERROR_CHECK(led_strip_refresh(strip));
}

static void set_state(led_strip_handle_t strip, robot_status_t state)
{
    status_display_set(state);
    switch (state) {
    case ROBOT_STATUS_IDLE:
        set_rgb(strip, 0, 0, 16);
        break;
    case ROBOT_STATUS_LISTENING:
        set_rgb(strip, 0, 24, 0);
        break;
    case ROBOT_STATUS_PLAYING:
        set_rgb(strip, 18, 8, 0);
        break;
    case ROBOT_STATUS_ERROR:
        set_rgb(strip, 24, 0, 0);
        break;
    }
}

static void log_resource_snapshot(const char *label)
{
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    ESP_LOGI(TAG,
             "RESOURCE %s: internal_free=%u internal_min=%u "
             "internal_largest=%u psram_free=%u psram_min=%u psram_largest=%u",
             label,
             (unsigned int)heap_caps_get_free_size(internal_caps),
             (unsigned int)heap_caps_get_minimum_free_size(internal_caps),
             (unsigned int)heap_caps_get_largest_free_block(internal_caps),
             (unsigned int)heap_caps_get_free_size(psram_caps),
             (unsigned int)heap_caps_get_minimum_free_size(psram_caps),
             (unsigned int)heap_caps_get_largest_free_block(psram_caps));
}

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
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    return config;
}

static i2s_chan_handle_t init_audio_output(void)
{
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, &tx, NULL));

    i2s_std_config_t std_config = make_i2s_config();
    std_config.gpio_cfg.din = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx, &std_config));

    size_t bytes_loaded = sizeof(tx_words);
    memset(tx_words, 0, sizeof(tx_words));
    while (bytes_loaded == sizeof(tx_words)) {
        ESP_ERROR_CHECK(i2s_channel_preload_data(
            tx, tx_words, sizeof(tx_words), &bytes_loaded));
    }
    ESP_ERROR_CHECK(i2s_channel_enable(tx));
    return tx;
}

static i2s_chan_handle_t init_microphone_input(void)
{
    i2s_chan_handle_t rx = NULL;
    const i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, NULL, &rx));

    i2s_std_config_t std_config = make_i2s_config();
    std_config.gpio_cfg.dout = I2S_GPIO_UNUSED;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &std_config));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));
    return rx;
}

static void write_tx_frame(i2s_chan_handle_t tx)
{
    size_t bytes_written = 0;
    ESP_ERROR_CHECK(i2s_channel_write(
        tx, tx_words, sizeof(tx_words), &bytes_written, 1000));
    ESP_ERROR_CHECK(bytes_written == sizeof(tx_words) ? ESP_OK : ESP_ERR_INVALID_SIZE);
}

static void write_silence(i2s_chan_handle_t tx, uint32_t duration_ms)
{
    memset(tx_words, 0, sizeof(tx_words));
    const uint32_t frames = (duration_ms + TX_FRAME_MS - 1) / TX_FRAME_MS;
    for (uint32_t frame = 0; frame < frames; ++frame) {
        write_tx_frame(tx);
    }
}

static void play_tone(i2s_chan_handle_t tx, uint32_t frequency_hz, uint32_t duration_ms)
{
    const uint32_t table_length = sizeof(SINE_TABLE) / sizeof(SINE_TABLE[0]);
    const uint32_t phase_modulus = table_length << 16;
    const uint32_t phase_increment =
        (uint32_t)(((uint64_t)frequency_hz * phase_modulus) / SAMPLE_RATE_HZ);
    uint32_t phase = 0;
    const uint32_t frames = (duration_ms + TX_FRAME_MS - 1) / TX_FRAME_MS;

    for (uint32_t frame = 0; frame < frames; ++frame) {
        for (size_t sample_index = 0;
             sample_index < TX_SAMPLES_PER_FRAME;
             ++sample_index) {
            const int32_t sample = (int32_t)SINE_TABLE[phase >> 16] << 16;
            tx_words[sample_index * 2] = sample;
            tx_words[sample_index * 2 + 1] = sample;
            phase = (phase + phase_increment) % phase_modulus;
        }
        write_tx_frame(tx);
    }
}

static void play_start_prompt(void)
{
    i2s_chan_handle_t tx = init_audio_output();
    write_silence(tx, 80);
    ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 1));
    play_tone(tx, 880, 220);
    write_silence(tx, 120);
    ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 0));
    ESP_ERROR_CHECK(i2s_channel_disable(tx));
    ESP_ERROR_CHECK(i2s_del_channel(tx));
}

static void record_microphone(int16_t *samples)
{
    i2s_chan_handle_t rx = init_microphone_input();
    for (int index = 0; index < RX_STARTUP_READS; ++index) {
        size_t bytes_read = 0;
        ESP_ERROR_CHECK(i2s_channel_read(
            rx, rx_words, sizeof(rx_words), &bytes_read, 1000));
    }

    size_t samples_written = 0;
    while (samples_written < RECORD_SAMPLES) {
        size_t bytes_read = 0;
        ESP_ERROR_CHECK(i2s_channel_read(
            rx, rx_words, sizeof(rx_words), &bytes_read, 1000));
        const size_t words_read = bytes_read / sizeof(rx_words[0]);
        for (size_t word = 0;
             word + 1 < words_read && samples_written < RECORD_SAMPLES;
             word += I2S_WORDS_PER_FRAME) {
            samples[samples_written++] = (int16_t)(rx_words[word] >> 16);
        }
    }

    ESP_ERROR_CHECK(i2s_channel_disable(rx));
    ESP_ERROR_CHECK(i2s_del_channel(rx));
}

static void normalize_recording(int16_t *samples)
{
    int16_t minimum = INT16_MAX;
    int16_t maximum = INT16_MIN;
    int64_t sum = 0;
    for (size_t index = 0; index < RECORD_SAMPLES; ++index) {
        if (samples[index] < minimum) {
            minimum = samples[index];
        }
        if (samples[index] > maximum) {
            maximum = samples[index];
        }
        sum += samples[index];
    }

    const int32_t mean = (int32_t)(sum / RECORD_SAMPLES);
    uint32_t clipped = 0;
    for (size_t index = 0; index < RECORD_SAMPLES; ++index) {
        int32_t amplified = ((int32_t)samples[index] - mean) * PLAYBACK_GAIN;
        if (amplified > INT16_MAX) {
            amplified = INT16_MAX;
            ++clipped;
        } else if (amplified < INT16_MIN) {
            amplified = INT16_MIN;
            ++clipped;
        }
        samples[index] = (int16_t)amplified;
    }

    ESP_LOGI(TAG,
             "Capture: samples=%u raw_min=%d raw_max=%d mean=%" PRId32
             " gain=%dx clipped=%" PRIu32,
             RECORD_SAMPLES,
             minimum,
             maximum,
             mean,
             PLAYBACK_GAIN,
             clipped);
}

static void play_recording(const int16_t *samples)
{
    i2s_chan_handle_t tx = init_audio_output();
    write_silence(tx, 80);
    ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 1));

    for (size_t offset = 0; offset < RECORD_SAMPLES; offset += TX_SAMPLES_PER_FRAME) {
        for (size_t frame_sample = 0;
             frame_sample < TX_SAMPLES_PER_FRAME;
             ++frame_sample) {
            const int32_t output = (int32_t)samples[offset + frame_sample] << 16;
            tx_words[frame_sample * 2] = output;
            tx_words[frame_sample * 2 + 1] = output;
        }
        write_tx_frame(tx);
    }

    write_silence(tx, 160);
    ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 0));
    ESP_ERROR_CHECK(i2s_channel_disable(tx));
    ESP_ERROR_CHECK(i2s_del_channel(tx));
}

static void init_button(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << BOARD_PTT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));
}

static void wait_for_level(int target_level)
{
    uint32_t stable_polls = 0;
    while (stable_polls < BUTTON_DEBOUNCE_POLLS) {
        if (gpio_get_level(BOARD_PTT_BUTTON_GPIO) == target_level) {
            ++stable_polls;
        } else {
            stable_polls = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

void app_main(void)
{
    led_strip_handle_t status_rgb = NULL;

    ESP_LOGI(TAG, "S2.3 minimal Wi-Fi/WakeNet/media coexistence started");
    ESP_LOGI(TAG, "Demo version: %s; board: %s", DEMO_VERSION, BOARD_NAME);

    ESP_ERROR_CHECK(gpio_reset_pin(BOARD_AMP_ENABLE_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(BOARD_AMP_ENABLE_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 0));
    ESP_ERROR_CHECK(create_onboard_rgb(&status_rgb));
    init_button();
    ESP_ERROR_CHECK(status_display_init());
    const esp_err_t wifi_result = wifi_manager_init();
    if (wifi_result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi manager unavailable; local PTT remains active: %s",
                 esp_err_to_name(wifi_result));
    }

    bool wakenet_ready = wakenet_listener_init() == ESP_OK;
    if (!wakenet_ready) {
        ESP_LOGE(TAG, "WakeNet unavailable; PTT remains active in explicit error mode");
    }

    int16_t *samples = heap_caps_malloc(
        RECORD_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (samples == NULL) {
        set_state(status_rgb, ROBOT_STATUS_ERROR);
        ESP_LOGE(TAG, "Unable to allocate recording buffer");
        return;
    }

    set_state(status_rgb, wakenet_ready ? ROBOT_STATUS_IDLE : ROBOT_STATUS_ERROR);
    log_resource_snapshot("startup-ready");
    if (wakenet_ready) {
        ESP_LOGI(TAG, "WAKE READY: say '你好小智' or press GPIO%d", BOARD_PTT_BUTTON_GPIO);
    } else {
        ESP_LOGI(TAG, "PTT FALLBACK: press GPIO%d button to record once",
                 BOARD_PTT_BUTTON_GPIO);
    }

    while (true) {
        bool wake_detected = false;
        bool ptt_pressed = false;

        if (wakenet_ready) {
            const esp_err_t listener_result = wakenet_listener_poll(&wake_detected);
            if (listener_result != ESP_OK) {
                ESP_LOGE(TAG, "WakeNet listener failed: %s", esp_err_to_name(listener_result));
                (void)wakenet_listener_pause();
                wakenet_ready = false;
                set_state(status_rgb, ROBOT_STATUS_ERROR);
                continue;
            }
            if (!wake_detected && gpio_get_level(BOARD_PTT_BUTTON_GPIO) != 0) {
                continue;
            }
            if (!wake_detected) {
                wait_for_level(0);
                ptt_pressed = true;
            }
            ESP_ERROR_CHECK(wakenet_listener_pause());
        } else {
            wait_for_level(0);
            ptt_pressed = true;
        }

        ESP_LOGI(TAG, "%s trigger; starting one record/playback cycle",
                 wake_detected ? "WakeNet" : "PTT");

        set_state(status_rgb, ROBOT_STATUS_LISTENING);
        play_start_prompt();
        vTaskDelay(pdMS_TO_TICKS(300));
        ESP_LOGI(TAG, "LISTENING: speak now for %d seconds", RECORD_SECONDS);
        record_microphone(samples);
        ESP_LOGI(TAG, "Recording complete");
        normalize_recording(samples);

        set_state(status_rgb, ROBOT_STATUS_PLAYING);
        ESP_LOGI(TAG, "PLAYING: local recording at +12 dB with saturation");
        play_recording(samples);

        if (ptt_pressed) {
            wait_for_level(1);
        }
        if (wakenet_ready) {
            const esp_err_t resume_result = wakenet_listener_resume();
            if (resume_result != ESP_OK) {
                ESP_LOGE(TAG, "WakeNet resume failed: %s", esp_err_to_name(resume_result));
                wakenet_ready = false;
            }
        }
        set_state(status_rgb, wakenet_ready ? ROBOT_STATUS_IDLE : ROBOT_STATUS_ERROR);
        log_resource_snapshot("post-cycle");
        if (wakenet_ready) {
            ESP_LOGI(TAG, "WAKE READY: cycle complete; say '你好小智' or press GPIO%d",
                     BOARD_PTT_BUTTON_GPIO);
        } else {
            ESP_LOGI(TAG, "PTT FALLBACK: cycle complete; press GPIO%d",
                     BOARD_PTT_BUTTON_GPIO);
        }
    }
}
