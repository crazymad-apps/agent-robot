#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "board_config.h"

#define MIC_SAMPLE_RATE_HZ 16000
#define MIC_DMA_FRAMES 256
#define MIC_WORDS_PER_FRAME 2
#define MIC_REPORT_FRAMES MIC_SAMPLE_RATE_HZ
#define MIC_STARTUP_READS 12

static const char *TAG = "agent_robot_mic";

/* Static storage keeps the app_main task stack small. Each frame is L then R. */
static int32_t rx_words[MIC_DMA_FRAMES * MIC_WORDS_PER_FRAME];

typedef struct {
    int64_t sum;
    uint64_t sum_squares;
    int16_t minimum;
    int16_t maximum;
    uint32_t samples;
} channel_stats_t;

static void reset_stats(channel_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
    stats->minimum = INT16_MAX;
    stats->maximum = INT16_MIN;
}

static void add_sample(channel_stats_t *stats, int16_t sample)
{
    stats->sum += sample;
    stats->sum_squares += (uint64_t)((int32_t)sample * (int32_t)sample);
    if (sample < stats->minimum) {
        stats->minimum = sample;
    }
    if (sample > stats->maximum) {
        stats->maximum = sample;
    }
    stats->samples++;
}

static double calculate_rms(const channel_stats_t *stats)
{
    if (stats->samples == 0) {
        return 0.0;
    }

    const double mean = (double)stats->sum / stats->samples;
    double variance = (double)stats->sum_squares / stats->samples - mean * mean;
    if (variance < 0.0) {
        variance = 0.0;
    }
    return sqrt(variance);
}

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

static void set_status_rgb(led_strip_handle_t strip, uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(strip, 0, red, green, blue));
    ESP_ERROR_CHECK(led_strip_refresh(strip));
}

static i2s_chan_handle_t init_microphone(void)
{
    i2s_chan_handle_t rx_channel = NULL;
    const i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, NULL, &rx_channel));

    const i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE_HZ),
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
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_channel, &std_config));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_channel));
    return rx_channel;
}

static void discard_startup_samples(i2s_chan_handle_t rx_channel)
{
    for (int read_index = 0; read_index < MIC_STARTUP_READS; ++read_index) {
        size_t bytes_read = 0;
        ESP_ERROR_CHECK(i2s_channel_read(
            rx_channel, rx_words, sizeof(rx_words), &bytes_read, 1000));
    }
}

static void log_report(uint32_t report_number,
                       const channel_stats_t *left,
                       const channel_stats_t *right)
{
    const double left_mean = (double)left->sum / left->samples;
    const double right_mean = (double)right->sum / right->samples;
    const double left_rms = calculate_rms(left);
    const double right_rms = calculate_rms(right);
    const bool left_is_active = left_rms >= right_rms;

    ESP_LOGI(TAG,
             "Window %" PRIu32 " | LEFT(expected) min=%d max=%d mean=%.1f rms=%.1f "
             "| RIGHT min=%d max=%d mean=%.1f rms=%.1f | active=%s",
             report_number,
             left->minimum,
             left->maximum,
             left_mean,
             left_rms,
             right->minimum,
             right->maximum,
             right_mean,
             right_rms,
             left_is_active ? "LEFT" : "RIGHT");
}

void run_mic_level_test(void)
{
    led_strip_handle_t status_rgb = NULL;

    ESP_LOGI(TAG, "Agent Robot single-device microphone test started");
    ESP_LOGI(TAG, "Demo version: %s; board: %s", DEMO_VERSION, BOARD_NAME);

    ESP_ERROR_CHECK(gpio_reset_pin(BOARD_AMP_ENABLE_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(BOARD_AMP_ENABLE_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 0));
    ESP_LOGI(TAG, "Amplifier held disabled on GPIO%d", BOARD_AMP_ENABLE_GPIO);

    ESP_ERROR_CHECK(create_onboard_rgb(&status_rgb));
    set_status_rgb(status_rgb, 24, 0, 0);

    ESP_LOGI(TAG,
             "Initializing INMP441: rate=%d Hz BCLK=GPIO%d WS=GPIO%d SD=GPIO%d",
             MIC_SAMPLE_RATE_HZ,
             BOARD_I2S_BCLK_GPIO,
             BOARD_I2S_WS_GPIO,
             BOARD_I2S_RX_GPIO);
    i2s_chan_handle_t rx_channel = init_microphone();
    discard_startup_samples(rx_channel);
    set_status_rgb(status_rgb, 0, 24, 0);

    ESP_LOGI(TAG, "Microphone ready; reporting DC-corrected RMS once per second");
    ESP_LOGI(TAG, "L/R is grounded, so LEFT should be the active slot");

    channel_stats_t left;
    channel_stats_t right;
    reset_stats(&left);
    reset_stats(&right);
    uint32_t report_number = 1;

    while (true) {
        size_t bytes_read = 0;
        const esp_err_t read_result = i2s_channel_read(
            rx_channel, rx_words, sizeof(rx_words), &bytes_read, 1000);
        if (read_result != ESP_OK) {
            ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(read_result));
            continue;
        }

        const size_t word_count = bytes_read / sizeof(rx_words[0]);
        for (size_t word_index = 0;
             word_index + 1 < word_count;
             word_index += MIC_WORDS_PER_FRAME) {
            /* INMP441 emits signed 24-bit samples left-aligned in each 32-bit slot. */
            add_sample(&left, (int16_t)(rx_words[word_index] >> 16));
            add_sample(&right, (int16_t)(rx_words[word_index + 1] >> 16));
        }

        if (left.samples >= MIC_REPORT_FRAMES) {
            log_report(report_number++, &left, &right);
            reset_stats(&left);
            reset_stats(&right);
        }
    }
}
