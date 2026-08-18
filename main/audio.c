/*
 * 音频通路（桩实现）。
 *
 * TODO(M1): 使用 I2S 驱动真实麦克风/喇叭。
 *   - 采集: ESP32-S3-BOX-3 双麦 或 INMP441（I2S slave）
 *   - 播放: BOX 自带功放 或 MAX98357A（I2S DAC）
 *   - 监听状态下: 采集任务按块将 PCM 通过 ws_send_bin() 上送，
 *     块间发送 audio_start / audio_end 信令（见 protocol.h）。
 */
#include <string.h>

#include "esp_log.h"

#include "audio.h"
#include "ws_client.h"

static const char *TAG = "audio";

static volatile bool s_listening = false;

void audio_init(void)
{
    /* TODO(M1): i2s_driver_install + 麦克风/功放初始化 */
    ESP_LOGI(TAG, "audio stub initialized (sample_rate=%d, ch=%d, bits=%d)",
             AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, AUDIO_BITS);
}

void audio_set_listening(bool on)
{
    s_listening = on;
    ESP_LOGI(TAG, "listening = %d", (int)on);
    /* TODO(M1): 启动/停止采集任务 */
}

bool audio_is_listening(void)
{
    return s_listening;
}

void audio_play_start(void)
{
    ESP_LOGI(TAG, "play start");
    /* TODO(M1): 打开播放缓冲，丢弃可能的残留数据 */
}

void audio_play(const uint8_t *data, size_t len)
{
    /* TODO(M1): i2s_write() 到功放/喇叭 */
    ESP_LOGI(TAG, "play %u bytes", (unsigned)len);
}

void audio_play_end(void)
{
    ESP_LOGI(TAG, "play end");
    /* TODO(M1): 刷空播放缓冲 */
}
