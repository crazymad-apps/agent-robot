# S3.0：Demo 链路验证

| 项 | 内容 |
|---|---|
| 状态 | 已完成：demo.1–demo.3、连续 20 轮 PTT 与连接/ASR/Agent/TTS 故障恢复均通过 |
| 开始日期 | 2026-08-22 |
| 所属路线 | 阶段三：Agent 接入 |
| 前置阶段 | S2.3：已完成 |
| 下一阶段 | S3.1：正式终端通信方案 |

## 本阶段目标

在不等待 ez-assistant 正式语音接入版本的前提下，用隔离的简易 Host 验证：

1. ESP32-S3 与 PC 之间的控制消息和 PCM 双向传输。
2. 完整 PTT 话语的 ASR 与 TTS 播放闭环。
3. ASR 文本通过 ez-assistant `agent-sdk` 临时执行入口产生回复，再由 TTS 播放。

## 当前决定

- Demo Host 使用 Rust，现归档于 [`demos/S3.0/voice-demo-host/`](../../../demos/S3.0/voice-demo-host/)。
- 终端使用从零建立的独立工程，现归档于 [`demos/S3.0/`](../../../demos/S3.0/)；不继承或兼容
  S1/S2 Demo 实现，也不作为 S3.1 的兼容基线。
- 原 S1/S2 累计工程冻结在 [`demos/S2.3/`](../../../demos/S2.3/)，只作为历史验收快照。
- Demo Host 位于 agent-robot，通过本地路径引用相邻 `ez-assistant` 仓库的 `agent-sdk`。
- 首轮使用无鉴权 `ws://`，设备连接后直接通信；不使用 token、配对或设备身份校验。
- 音频为 PCM16 little-endian、16 kHz、mono；PTT 松开结束本轮话语。
- 首轮只支持单设备和内存临时状态，不提供正式 Session、审批、恢复或多设备语义。
- Demo 协议不进入 `assistant-protocol`，验证完成后可以被替换。
- ASR/TTS 选用 DashScope 北京地域整句 HTTP API；SSE 不进入终端通信链路。
- 固件不保留旧录音回放的数字增益；当前按用户试听结果将 TTS PCM 衰减到 75% 播放。
- Agent 使用 ez-assistant `agent-sdk::Agent::start_ephemeral`，通过其
  `agent-openai-compatible` adapter 调用 DashScope `qwen-plus`；不复制 Agent Loop。

无鉴权和明文 WebSocket 只允许用于当前受控局域网开发验证，不是正式产品安全基线。

## 入口

- [`阶段计划.md`](阶段计划.md)
- [`技术选型.md`](技术选型.md)
- [`验收记录.md`](验收记录.md)
- [`../../../demos/S3.0/voice-demo-host/README.md`](../../../demos/S3.0/voice-demo-host/README.md)
- [`../../../demos/S3.0/README.md`](../../../demos/S3.0/README.md)
- [`../../../demos/README.md`](../../../demos/README.md)

## 当前产物

- Voice Demo Host Rust crate。
- `/health` 健康检查和 `/ws` WebSocket 入口。
- `hello`、`listen_start`、`listen_stop`、`playback_cancel`、`ping` 最小控制消息。
- 裸 PCM 上送、大小限制和状态控制。
- ESP32-S3 PTT 录音、无鉴权 WebSocket PCM 上送、回传缓冲和 I2S 播放固件。
- DashScope ASR/TTS Provider 抽象、真实调用和 PCM 回传。
- `agent-sdk` ephemeral Agent、DashScope `qwen-plus` 模型装配、20 秒取消超时和播报长度限制。
