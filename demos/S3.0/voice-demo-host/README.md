# Voice Demo Host

S3.0 的临时 Rust 验证宿主。当前已实现 `demo.3` 首轮链路：ESP32 上传一段
PCM16/16 kHz/mono 音频，Host 调用 DashScope ASR，将识别结果交给 ez-assistant
`agent-sdk` 的临时执行入口，再调用 DashScope TTS 并把 PCM 分块回传。

它不是 ez-assistant Runtime Host，不提供正式 Session、Run、Conversation、审批、持久化或恢复。
当前 `ws://` 端点无 token、无配对、无 TLS，连接后直接通信，只能用于受控局域网开发环境。

## 目录关系

默认假设两个仓库位于同一父目录：

```text
github/
├── agent-robot/
└── ez-assistant/
```

`Cargo.toml` 通过本地路径直接引用 `ez-assistant` 的 `agent-sdk`、
`agent-openai-compatible` 和必要领域类型。构建会验证当前 ez-assistant 工作树可以直接链接：

```bash
cargo check --locked
```

## 启动

在本目录创建不会提交到 Git 的 `.env`：

```dotenv
DASHSCOPE_API_KEY=填写本地密钥
```

可选覆盖项为 `DASHSCOPE_BASE_URL`、`DASHSCOPE_ASR_MODEL`、`DASHSCOPE_TTS_MODEL` 和
`DASHSCOPE_TTS_VOICE`。默认使用北京地域公共端点、`qwen-audio-3.0-asr-flash`、
`qwen-audio-3.0-tts-flash` 和 `longanhuan_v3.6`。密钥只由 Host 读取，不进入设备、协议或普通日志。

Agent 默认通过 DashScope OpenAI-compatible 端点调用 `qwen-plus`。可选覆盖项为
`DASHSCOPE_LLM_BASE_URL`、`DASHSCOPE_LLM_MODEL`、`DASHSCOPE_LLM_CONTEXT_WINDOW_TOKENS` 和
`VOICE_DEMO_AGENT_TIMEOUT_SECONDS`；最后一项允许 1–60 秒，默认 20 秒。

```bash
cd demos/S3.0/voice-demo-host
cargo run
```

默认监听：

- 健康检查：`http://0.0.0.0:8787/health`
- WebSocket：`ws://0.0.0.0:8787/ws`

可以覆盖监听地址：

```bash
VOICE_DEMO_BIND=192.168.31.10:8787 cargo run
```

终端必须连接运行 Host 的 PC 局域网地址，不能使用 `0.0.0.0` 作为目标地址。

## Demo 私有协议

控制消息使用 JSON 文本帧，PCM 使用裸 WebSocket 二进制帧。协议版本为 `0.1`，表示尚未冻结。

连接后可先发送：

```json
{"type":"hello","protocol_major":0,"protocol_minor":1,"device_id":"esp32-demo-1"}
```

一轮 ASR/TTS：

```text
设备 -> {"type":"listen_start","utterance_id":"u-1"}
设备 -> <binary PCM chunks>
设备 -> {"type":"listen_stop","utterance_id":"u-1"}

Host -> {"type":"playback_start", ...}
Host -> <binary PCM chunks>
Host -> {"type":"playback_end", ...}
Host -> {"type":"state_changed","state":"idle"}
```

在播放前 Host 还会发送 `processing/asr`、`transcript`、`processing/agent`、
`assistant_text` 和 `processing/tts` 控制消息。每轮 Agent 使用独立的 ephemeral Recorder、
空工具集和单模型 step；只提取用户可见文本，播报最多 36 个字符。

其他控制消息：

- `{"type":"ping","nonce":"..."}`
- `{"type":"playback_cancel","audio_stream_id":"..."}`

单轮最大 60 秒，即 1,920,000 bytes。当前裸 PCM 没有正式二进制帧头；帧序号、幂等、
WSS、设备身份和兼容协商应根据 Demo 证据在正式协议中设计。

## 当前限制

- 可中断的异步播放队列
- 流式 ASR/TTS 和音频重采样
- Agent 多轮会话、工具、正式 Recorder、审批与恢复
- 正式鉴权、WSS、设备配对和多设备管理

ESP32 WebSocket 客户端与本 Host 一同归档在 [`../`](../)。媒体回环、真实 ASR/TTS、
`agent-sdk`、连续 20 轮 PTT 和故障恢复均已通过；本目录冻结为 S3.0 历史证据，不再继续演进。
