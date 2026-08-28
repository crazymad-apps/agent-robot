#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

#include "board_config.h"

#define AUDIO_SAMPLE_RATE_HZ 16000
#define AUDIO_FRAME_MS 20
#define AUDIO_SAMPLES_PER_FRAME ((AUDIO_SAMPLE_RATE_HZ * AUDIO_FRAME_MS) / 1000)
#define AUDIO_CHANNELS 2
#define AUDIO_WORDS_PER_FRAME (AUDIO_SAMPLES_PER_FRAME * AUDIO_CHANNELS)
#define NOTE_GAP_MS 80
#define AUDIO_FLUSH_MS 200
#define MELODY_PAUSE_MS 1500

static const char *TAG = "agent_robot_demo";

/*
 * Sine lookup table for direct digital synthesis. Peak amplitude is
 * 512/32768 (-36 dBFS). PCM16 values are left-aligned into 32-bit I2S slots
 * and duplicated to L/R.
 */
static const int16_t SINE_TABLE[] = {
    0, 196, 362, 473, 512, 473, 362, 196,
    0, -196, -362, -473, -512, -473, -362, -196,
};

typedef struct {
    uint16_t frequency_hz;
    uint16_t duration_ms;
} melody_note_t;

/* First two phrases of the public-domain melody "Twinkle, Twinkle, Little Star". */
static const melody_note_t MELODY[] = {
    {262, 350}, {262, 350}, {392, 350}, {392, 350},
    {440, 350}, {440, 350}, {392, 700},
    {349, 350}, {349, 350}, {330, 350}, {330, 350},
    {294, 350}, {294, 350}, {262, 700},
};

/* Keep the DMA feed buffers off app_main's small task stack. */
static int32_t audio_frame[AUDIO_WORDS_PER_FRAME];
static const int32_t silence_frame[AUDIO_WORDS_PER_FRAME] = {0};

static void print_board_info(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "ESP-IDF version: %s", esp_get_idf_version());
    ESP_LOGI(TAG,
             "Chip: ESP32-S3, cores: %d, silicon revision: %d.%d",
             chip_info.cores,
             chip_info.revision / 100,
             chip_info.revision % 100);

    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "Detected flash: %" PRIu32 " MB", flash_size / (1024 * 1024));
    }

    ESP_LOGI(TAG,
             "PSRAM total/free: %u/%u bytes",
             (unsigned int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
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

static i2s_chan_handle_t init_audio_output(void)
{
    i2s_chan_handle_t tx_channel = NULL;

    ESP_ERROR_CHECK(gpio_reset_pin(BOARD_AMP_ENABLE_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(BOARD_AMP_ENABLE_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 0));
    ESP_LOGI(TAG, "Amplifier disabled on GPIO%d during I2S setup", BOARD_AMP_ENABLE_GPIO);

    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.auto_clear_after_cb = true;
    ESP_ERROR_CHECK(i2s_new_channel(&channel_config, &tx_channel, NULL));

    const i2s_std_config_t std_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_BCLK_GPIO,
            .ws = BOARD_I2S_WS_GPIO,
            .dout = BOARD_I2S_TX_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_channel, &std_config));

    size_t bytes_loaded = sizeof(silence_frame);
    while (bytes_loaded == sizeof(silence_frame)) {
        ESP_ERROR_CHECK(i2s_channel_preload_data(
            tx_channel, silence_frame, sizeof(silence_frame), &bytes_loaded));
    }
    ESP_ERROR_CHECK(i2s_channel_enable(tx_channel));

    ESP_LOGI(TAG,
             "I2S TX ready: 16 kHz, 32-bit stereo slots; BCLK=%d WS=%d DIN=%d",
             BOARD_I2S_BCLK_GPIO,
             BOARD_I2S_WS_GPIO,
             BOARD_I2S_TX_GPIO);
    return tx_channel;
}

static void fill_tone_frame(int32_t *frame, uint32_t frequency_hz, uint32_t *phase)
{
    const uint32_t table_length = sizeof(SINE_TABLE) / sizeof(SINE_TABLE[0]);
    const uint32_t phase_modulus = table_length << 16;
    const uint32_t phase_increment =
        (uint32_t)(((uint64_t)frequency_hz * phase_modulus) / AUDIO_SAMPLE_RATE_HZ);

    for (size_t sample_index = 0; sample_index < AUDIO_SAMPLES_PER_FRAME; ++sample_index) {
        int32_t sample = (int32_t)SINE_TABLE[*phase >> 16] << 16;
        frame[sample_index * AUDIO_CHANNELS] = sample;
        frame[sample_index * AUDIO_CHANNELS + 1] = sample;
        *phase += phase_increment;
        if (*phase >= phase_modulus) {
            *phase -= phase_modulus;
        }
    }
}

static void write_audio_frame(i2s_chan_handle_t tx_channel, const int32_t *frame)
{
    size_t bytes_written = 0;
    const size_t expected_bytes = AUDIO_WORDS_PER_FRAME * sizeof(frame[0]);

    ESP_ERROR_CHECK(i2s_channel_write(
        tx_channel, frame, expected_bytes, &bytes_written, pdMS_TO_TICKS(1000)));
    ESP_ERROR_CHECK(bytes_written == expected_bytes ? ESP_OK : ESP_ERR_INVALID_SIZE);
}

static void write_silence(i2s_chan_handle_t tx_channel, uint32_t duration_ms)
{
    memset(audio_frame, 0, sizeof(audio_frame));
    const uint32_t frame_count =
        (duration_ms + AUDIO_FRAME_MS - 1) / AUDIO_FRAME_MS;

    for (uint32_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        write_audio_frame(tx_channel, audio_frame);
    }
}

static void play_note(i2s_chan_handle_t tx_channel, const melody_note_t *note)
{
    uint32_t phase = 0;
    const uint32_t frame_count =
        (note->duration_ms + AUDIO_FRAME_MS - 1) / AUDIO_FRAME_MS;

    for (uint32_t frame_index = 0; frame_index < frame_count; ++frame_index) {
        fill_tone_frame(audio_frame, note->frequency_hz, &phase);
        write_audio_frame(tx_channel, audio_frame);
    }
    write_silence(tx_channel, NOTE_GAP_MS);
}

void run_audio_test(void)
{
    led_strip_handle_t status_rgb = NULL;
    unsigned int cycle = 0;

    ESP_LOGI(TAG, "Agent Robot single-device audio test started");
    ESP_LOGI(TAG, "Demo version: %s; board: %s", DEMO_VERSION, BOARD_NAME);
    print_board_info();

    ESP_ERROR_CHECK(create_onboard_rgb(&status_rgb));
    set_status_rgb(status_rgb, 24, 0, 0);

    i2s_chan_handle_t tx_channel = init_audio_output();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 1));
    set_status_rgb(status_rgb, 0, 24, 0);
    ESP_LOGI(TAG, "Amplifier enabled; beginning low-level melody test");

    while (true) {
        ++cycle;
        ESP_LOGI(TAG, "Melody cycle %u: %u notes at -36 dBFS", cycle,
                 (unsigned int)(sizeof(MELODY) / sizeof(MELODY[0])));
        for (size_t note_index = 0; note_index < sizeof(MELODY) / sizeof(MELODY[0]);
             ++note_index) {
            play_note(tx_channel, &MELODY[note_index]);
        }

        write_silence(tx_channel, AUDIO_FLUSH_MS);
        ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 0));
        ESP_LOGI(TAG, "Amplifier disabled between melody cycles for %d ms", MELODY_PAUSE_MS);
        vTaskDelay(pdMS_TO_TICKS(MELODY_PAUSE_MS));
        ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 1));
    }
}
