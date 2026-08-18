#ifndef AGENT_ROBOT_AUDIO_H
#define AGENT_ROBOT_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** 音频采样率（与远端 Agent 协商，见协议文档） */
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_CHANNELS    1
#define AUDIO_BITS        16

/** 初始化音频通路（I2S 采集/播放）。M0 阶段为桩实现 */
void audio_init(void);

/** 控制是否处于监听状态（唤醒/会话开始后置 true） */
void audio_set_listening(bool on);

bool audio_is_listening(void);

/** 播放一段音频（来自远端 TTS 结果） */
void audio_play(const uint8_t *data, size_t len);

/** 一段播放开始/结束（用于状态切换与缓冲管理） */
void audio_play_start(void);
void audio_play_end(void);

#endif /* AGENT_ROBOT_AUDIO_H */
