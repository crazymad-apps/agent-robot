# 软件部分：Agent 通信与交互（初稿）

> 本文档是 agent-robot 方案的**软件层**，聚焦**终端 ↔ Agent 的通信**与**交互设计**。
> 硬件层（元器件选型）见 [`hardware.md`](hardware.md)，项目总览见 [`architecture.md`](architecture.md)。
>
> ⚠️ **初稿说明**：本文档为讨论阶段的草案，多处为推荐方向并标注「待确认」，
> 会随 Agent 本体（ez-assistant）的演进持续修改。

## 1. 终端软件定位

终端是 **Agent 本体的远端 UI 客户端**（UI 层 + 操作层）：

- Agent 本体 = **ez-assistant**（Rust，`~/github/ez-assistant`，开发中，不代表最终形态），
  业务权威状态（Session / Run / Conversation / 记忆 / 权限）由它的 Runtime 持有。
- 本终端 = 一个**远端语音 UI 客户端**，与桌面端是同一 Runtime 的不同形态客户端，
  只负责：监听（麦克风）、发声（喇叭）、显示（屏幕）、交互（按键/唤醒）。

## 2. 整体拓扑（推荐方向，待确认）

```
┌─ 终端硬件（ESP32-S3，本项目）──────────────────────┐
│  采集 PCM → WS 上送 │ WS 收音频帧 → 播放          │
│  唤醒词(ESP-SR) / 按键 / LVGL 显示 / 状态机       │
└───────────────▲───────────────────────────────────┘
                │ WebSocket（终端↔网关私有信令）
┌───────────────┴───────────────────────────────────┐
│  网关 Gateway（桥接 + 语音能力）                    │
│  ├─ ASR：语音 → 文本          （待确认选型）        │
│  ├─ TTS：文本 → 语音          （待确认选型）        │
│  └─ 以 Runtime Client 身份对接本体私用协议          │
└───────────────▲───────────────────────────────────┘
                │ assistant-protocol（本地 HTTP/SSE）
┌───────────────┴───────────────────────────────────┐
│  ez-assistant Runtime Host（localhost，权威状态）  │
└───────────────────────────────────────────────────┘
```

- **Runtime Host**：只监听 127.0.0.1，Bearer Token + Host/CORS 白名单鉴权（本地私用设计）。
- **网关**：本机以 Runtime Client 身份连 Runtime Host（复用现有私用协议），
  对外开 WebSocket 给终端，并承载 ASR/TTS——**本体零改动**。
- 部署形态：**本地起步**（PC 跑 Runtime + 网关，终端连局域网）为推荐方向（待确认）；
  远程部署会改变本体 local-first 语义（本地文件/Shell 工具、本地存储），暂不规划。

## 3. Agent 通信

### 3.1 协议语义（以 assistant-protocol 为准）

不另造协议，终端→网关→Runtime 全链路沿用 **assistant-protocol 语义**：

| 动作 | 命令/事件 |
|---|---|
| 获取全局快照 | `GetApplicationSnapshot`（含 sessions/models/workspaces/capabilities） |
| 建会话 | `CreateSession` |
| 提交用户输入 | `SubmitInput{session_id, message, variant, idempotency_key}`（文本） |
| 会话列表/详情 | `ListSessions` / `GetSessionView` |
| 取消/重试 | `CancelRun` / `RetryRun` |
| 流式正文 | 事件 `TextDelta` / `ReasoningDelta`（SSE） |
| 工具与审批 | `ToolProposed` / `ApprovalRequested`（语音终端先不做审批交互，见 §7） |

**关键约束**：事件可丢失（背压/断线），客户端必须以快照水位（`ObservedSnapshot.observed_sequence`）对齐，重连后重新拉快照，不能简单重放事件。

### 3.2 终端 ↔ 网关的 WS 信令（私有，草案）

| 方向 | 消息 | 说明 |
|---|---|---|
| 终端→网关 | `hello` | 设备标识、音频参数（采样率 16k/16bit/单声道） |
| 终端→网关 | `audio_data`（二进制） | PCM 音频帧上送（可后续换 Opus） |
| 终端→网关 | `audio_end` | 一句话结束（触发 ASR） |
| 网关→终端 | `state` | idle / listening / thinking / speaking |
| 网关→终端 | `text` | 对话文本（上屏） |
| 网关→终端 | `audio_data`（二进制） | TTS 音频帧回传（播放） |
| 网关→终端 | `audio_end` | 一段播放结束 |
| 双向 | `ping/pong` | 心跳保活 |

### 3.3 音频通路（推荐方向）

```
用户说话 → 终端采集 PCM → WS → 网关 ASR → 文本 → SubmitInput
                                              ↓
         终端播放 ← WS ← 网关 TTS ← TextDelta 流式文本
```

- 上送：16kHz 16bit 单声道 PCM（简单优先）；带宽敏感时再考虑 Opus（待确认）。
- 回传：TTS 音频帧，按块播放，低延迟优先。

### 3.4 心跳与重连

- 心跳保活 + 指数退避重连；断网状态上屏。
- 重连后重新拉取快照对齐水位，恢复会话上下文。

## 4. 交互设计

### 4.1 会话状态机

```
disconnected ──连接成功──▶ connected ──唤醒/按键──▶ listening
     ▲                        ▲                        │ 语音结束
     └──────断线重连──────────┘                        ▼
                          speaking ◀──TTS 播放── thinking ◀──提交成功
```

- `listening`：终端采集上送，网关 ASR 中
- `thinking`：Runtime 推理中（等待首段 TextDelta）
- `speaking`：TTS 音频播放中（可按键打断）

### 4.2 唤醒

- **ESP-SR WakeNet 离线唤醒词**（先官方内置词，后续可训练自定义词），唤醒后进入监听。
- 备用：按键 PTT 触发监听。

### 4.3 会话管理（窄接口起步）

- 终端只做**语音对话链路**：唤醒 → 说话 → 回复 → 上屏。
- 审批（Ask 模式）、记忆管理、历史搜索、Workspace 等先不做（桌面端已覆盖）。

## 5. 显示 UI

- **LVGL + ST7789 小屏**：状态（idle/listening/thinking/speaking）+ 对话文本。
- 可选：ESP32 内置 Web 页面（手机/浏览器查看），作为扩展。

## 6. 软件学习路线

- **L3 联调**：飞线原型 → 连本地 ez-assistant（Runtime Host + 网关）端到端语音对话
- **网关开发**：assistant-protocol 客户端（命令/事件）+ WebSocket 服务 + ASR/TTS 接入
- **终端固件**：WS 客户端、音频采集/播放、LVGL 界面、状态机、断线重连

## 7. 待确认项（影响本文档后续修改）

1. **网关语言**：Rust（可复用 assistant-protocol crate，协议自动同步）？还是 Python（开发快）？
2. **ASR/TTS 选型**：本地 Whisper / 云端 API / 其他？
3. **部署拓扑**：本地起步（推荐）是否确认？
4. **能力边界**：窄接口（仅语音对话链路）是否确认？
5. **音频压缩**：PCM 起步，是否需要 Opus？
