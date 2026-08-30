#include "audio_hardware.h"

#include <stdbool.h>
#include <stdlib.h>

#include "agent_robot_board.h"
#include "audio_pcm_conversion.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_err.h"

#define AUDIO_HARDWARE_SAMPLE_RATE_HZ 16000U
#define AUDIO_HARDWARE_DMA_DESCRIPTOR_COUNT 8U
#define AUDIO_HARDWARE_DMA_FRAME_COUNT 320U
#define AUDIO_HARDWARE_SILENCE_WORD_COUNT                                                          \
    (AUDIO_HARDWARE_DMA_FRAME_COUNT * AUDIO_PCM_I2S_WORDS_PER_FRAME)

struct audio_hardware {
    i2s_chan_handle_t transmit_channel;
    i2s_chan_handle_t receive_channel;
    bool capture_enabled;
    bool playback_enabled;
};

static const int32_t playback_silence[AUDIO_HARDWARE_SILENCE_WORD_COUNT];

static audio_hardware_result_t driver_result(esp_err_t result)
{
    if (result == ESP_OK) {
        return AUDIO_HARDWARE_RESULT_OK;
    }
    if (result == ESP_ERR_TIMEOUT) {
        return AUDIO_HARDWARE_RESULT_TIMEOUT;
    }
    if (result == ESP_ERR_INVALID_ARG) {
        return AUDIO_HARDWARE_RESULT_INVALID_ARGUMENT;
    }
    if (result == ESP_ERR_INVALID_STATE) {
        return AUDIO_HARDWARE_RESULT_INVALID_STATE;
    }
    if (result == ESP_ERR_NO_MEM) {
        return AUDIO_HARDWARE_RESULT_NO_MEMORY;
    }
    return AUDIO_HARDWARE_RESULT_DRIVER_FAILURE;
}

static void release_channels(audio_hardware_t *hardware)
{
    if (hardware->receive_channel != NULL) {
        (void)i2s_del_channel(hardware->receive_channel);
        hardware->receive_channel = NULL;
    }
    if (hardware->transmit_channel != NULL) {
        (void)i2s_del_channel(hardware->transmit_channel);
        hardware->transmit_channel = NULL;
    }
}

audio_hardware_result_t audio_hardware_create(audio_hardware_t **hardware)
{
    if (hardware == NULL || *hardware != NULL) {
        return AUDIO_HARDWARE_RESULT_INVALID_ARGUMENT;
    }
    if (gpio_reset_pin(AGENT_ROBOT_BOARD_AMPLIFIER_ENABLE_GPIO) != ESP_OK ||
        gpio_set_direction(AGENT_ROBOT_BOARD_AMPLIFIER_ENABLE_GPIO, GPIO_MODE_OUTPUT) != ESP_OK ||
        gpio_set_level(AGENT_ROBOT_BOARD_AMPLIFIER_ENABLE_GPIO, 0) != ESP_OK) {
        return AUDIO_HARDWARE_RESULT_DRIVER_FAILURE;
    }
    audio_hardware_t *created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        return AUDIO_HARDWARE_RESULT_NO_MEMORY;
    }
    i2s_chan_config_t channel_config = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_config.dma_desc_num = AUDIO_HARDWARE_DMA_DESCRIPTOR_COUNT;
    channel_config.dma_frame_num = AUDIO_HARDWARE_DMA_FRAME_COUNT;
    channel_config.auto_clear_after_cb = true;
    esp_err_t result =
        i2s_new_channel(&channel_config, &created->transmit_channel, &created->receive_channel);
    const i2s_std_config_t standard_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_HARDWARE_SAMPLE_RATE_HZ),
        .slot_cfg =
            I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = AGENT_ROBOT_BOARD_I2S_BCLK_GPIO,
                .ws = AGENT_ROBOT_BOARD_I2S_WS_GPIO,
                .dout = AGENT_ROBOT_BOARD_I2S_TX_GPIO,
                .din = AGENT_ROBOT_BOARD_I2S_RX_GPIO,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };
    if (result == ESP_OK) {
        result = i2s_channel_init_std_mode(created->transmit_channel, &standard_config);
    }
    if (result == ESP_OK) {
        result = i2s_channel_init_std_mode(created->receive_channel, &standard_config);
    }
    if (result != ESP_OK) {
        release_channels(created);
        free(created);
        (void)gpio_set_level(AGENT_ROBOT_BOARD_AMPLIFIER_ENABLE_GPIO, 0);
        return driver_result(result);
    }
    *hardware = created;
    return AUDIO_HARDWARE_RESULT_OK;
}

audio_hardware_result_t audio_hardware_capture_start(audio_hardware_t *hardware)
{
    if (hardware == NULL) {
        return AUDIO_HARDWARE_RESULT_INVALID_ARGUMENT;
    }
    if (hardware->capture_enabled || hardware->playback_enabled) {
        return AUDIO_HARDWARE_RESULT_INVALID_STATE;
    }
    if (gpio_set_level(AGENT_ROBOT_BOARD_AMPLIFIER_ENABLE_GPIO, 0) != ESP_OK) {
        return AUDIO_HARDWARE_RESULT_DRIVER_FAILURE;
    }
    const audio_hardware_result_t result =
        driver_result(i2s_channel_enable(hardware->receive_channel));
    if (result == AUDIO_HARDWARE_RESULT_OK) {
        hardware->capture_enabled = true;
    }
    return result;
}

audio_hardware_result_t audio_hardware_capture_read(audio_hardware_t *hardware, int32_t *i2s_words,
                                                    size_t i2s_word_capacity,
                                                    size_t *i2s_word_count, uint32_t timeout_ms)
{
    if (hardware == NULL || i2s_words == NULL || i2s_word_capacity == 0U ||
        i2s_word_capacity % AUDIO_PCM_I2S_WORDS_PER_FRAME != 0U || i2s_word_count == NULL ||
        i2s_word_capacity > SIZE_MAX / sizeof(*i2s_words)) {
        return AUDIO_HARDWARE_RESULT_INVALID_ARGUMENT;
    }
    if (!hardware->capture_enabled) {
        return AUDIO_HARDWARE_RESULT_INVALID_STATE;
    }
    size_t bytes_read = 0U;
    const audio_hardware_result_t result = driver_result(
        i2s_channel_read(hardware->receive_channel, i2s_words,
                         i2s_word_capacity * sizeof(*i2s_words), &bytes_read, timeout_ms));
    if (result != AUDIO_HARDWARE_RESULT_OK) {
        return result;
    }
    if (bytes_read == 0U ||
        bytes_read % (sizeof(*i2s_words) * AUDIO_PCM_I2S_WORDS_PER_FRAME) != 0U) {
        return AUDIO_HARDWARE_RESULT_DRIVER_FAILURE;
    }
    *i2s_word_count = bytes_read / sizeof(*i2s_words);
    return AUDIO_HARDWARE_RESULT_OK;
}

audio_hardware_result_t audio_hardware_capture_stop(audio_hardware_t *hardware)
{
    if (hardware == NULL) {
        return AUDIO_HARDWARE_RESULT_INVALID_ARGUMENT;
    }
    if (!hardware->capture_enabled) {
        return AUDIO_HARDWARE_RESULT_INVALID_STATE;
    }
    const audio_hardware_result_t result =
        driver_result(i2s_channel_disable(hardware->receive_channel));
    if (result == AUDIO_HARDWARE_RESULT_OK) {
        hardware->capture_enabled = false;
    }
    return result;
}

audio_hardware_result_t audio_hardware_playback_start(audio_hardware_t *hardware)
{
    if (hardware == NULL) {
        return AUDIO_HARDWARE_RESULT_INVALID_ARGUMENT;
    }
    if (hardware->capture_enabled || hardware->playback_enabled) {
        return AUDIO_HARDWARE_RESULT_INVALID_STATE;
    }
    if (gpio_set_level(AGENT_ROBOT_BOARD_AMPLIFIER_ENABLE_GPIO, 0) != ESP_OK) {
        return AUDIO_HARDWARE_RESULT_DRIVER_FAILURE;
    }
    size_t bytes_loaded = 0U;
    esp_err_t result = i2s_channel_preload_data(hardware->transmit_channel, playback_silence,
                                                sizeof(playback_silence), &bytes_loaded);
    if (result == ESP_OK && bytes_loaded == 0U) {
        result = ESP_FAIL;
    }
    if (result == ESP_OK) {
        result = i2s_channel_enable(hardware->transmit_channel);
    }
    if (result == ESP_OK) {
        result = gpio_set_level(AGENT_ROBOT_BOARD_AMPLIFIER_ENABLE_GPIO, 1);
    }
    if (result != ESP_OK) {
        (void)gpio_set_level(AGENT_ROBOT_BOARD_AMPLIFIER_ENABLE_GPIO, 0);
        (void)i2s_channel_disable(hardware->transmit_channel);
        return driver_result(result);
    }
    hardware->playback_enabled = true;
    return AUDIO_HARDWARE_RESULT_OK;
}

audio_hardware_result_t audio_hardware_playback_write(audio_hardware_t *hardware,
                                                      const int32_t *i2s_words,
                                                      size_t i2s_word_count, uint32_t timeout_ms)
{
    if (hardware == NULL || i2s_words == NULL || i2s_word_count == 0U ||
        i2s_word_count % AUDIO_PCM_I2S_WORDS_PER_FRAME != 0U ||
        i2s_word_count > SIZE_MAX / sizeof(*i2s_words)) {
        return AUDIO_HARDWARE_RESULT_INVALID_ARGUMENT;
    }
    if (!hardware->playback_enabled) {
        return AUDIO_HARDWARE_RESULT_INVALID_STATE;
    }
    const size_t expected_bytes = i2s_word_count * sizeof(*i2s_words);
    size_t bytes_written = 0U;
    const audio_hardware_result_t result = driver_result(i2s_channel_write(
        hardware->transmit_channel, i2s_words, expected_bytes, &bytes_written, timeout_ms));
    if (result != AUDIO_HARDWARE_RESULT_OK) {
        return result;
    }
    return bytes_written == expected_bytes ? AUDIO_HARDWARE_RESULT_OK
                                           : AUDIO_HARDWARE_RESULT_DRIVER_FAILURE;
}

audio_hardware_result_t audio_hardware_playback_stop(audio_hardware_t *hardware)
{
    if (hardware == NULL) {
        return AUDIO_HARDWARE_RESULT_INVALID_ARGUMENT;
    }
    if (!hardware->playback_enabled) {
        return AUDIO_HARDWARE_RESULT_INVALID_STATE;
    }
    const esp_err_t mute_result = gpio_set_level(AGENT_ROBOT_BOARD_AMPLIFIER_ENABLE_GPIO, 0);
    const esp_err_t disable_result = i2s_channel_disable(hardware->transmit_channel);
    if (disable_result == ESP_OK) {
        hardware->playback_enabled = false;
    }
    if (mute_result != ESP_OK) {
        return AUDIO_HARDWARE_RESULT_DRIVER_FAILURE;
    }
    return driver_result(disable_result);
}

void audio_hardware_destroy(audio_hardware_t *hardware)
{
    if (hardware == NULL) {
        return;
    }
    (void)gpio_set_level(AGENT_ROBOT_BOARD_AMPLIFIER_ENABLE_GPIO, 0);
    if (hardware->capture_enabled) {
        (void)i2s_channel_disable(hardware->receive_channel);
    }
    if (hardware->playback_enabled) {
        (void)i2s_channel_disable(hardware->transmit_channel);
    }
    release_channels(hardware);
    free(hardware);
}
