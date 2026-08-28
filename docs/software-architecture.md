# Agent 硬件终端：软件架构设计

> **文档状态：当前软件架构基线。**
>
> 本文固定 Runtime、Runtime Host 内置 Device Gateway / SpeechService 和终端之间的职责。
> 当前以 ez-assistant v0.21.0 联调工作树及 device protocol v1.0 为暂时冻结基线；联调发现缺口时
> 可以同步修改两仓实现与协议 fixtures。S3.0 Voice Demo Host 已冻结归档，不进入正式架构。

## 1. 设计目标

软件系统需要把 ESP32-S3 终端接入本地 ez-assistant，同时保持 Runtime 的权威状态和
local-first 边界：

- 终端只处理设备 I/O、媒体缓冲、连接和展示。
- ez-assistant Runtime 继续持有全部 Agent 业务状态。
- 语音和设备能力通过窄接入层适配，不复制第二套 Runtime Host。
- 断线、重试和进程重启不能导致重复输入或重复朗读。
- 首版优先完成 PTT 半双工语音闭环。

## 2. 系统上下文

```text
┌──────────────────────────────────────────────────────────┐
│ ESP32-S3 终端                                            │
│ 采集 / 播放 / 屏幕 / 按键 / 本地状态 / 设备连接          │
└───────────────────────▲──────────────────────────────────┘
                        │ 设备媒体协议（控制消息 + PCM）
┌───────────────────────┴──────────────────────────────────┐
│ ez-assistant Runtime Host                                │
│ Device Gateway / SpeechService / LAN WSS / ASR / TTS     │
│ Desktop loopback HTTP Command + SSE                      │
└───────────────────────▲──────────────────────────────────┘
                        │ 进程内正式 Runtime API
┌───────────────────────┴──────────────────────────────────┐
│ ez-assistant Runtime                                     │
│ Session / Run / Conversation / Queue / Approval / 权限    │
└──────────────────────────────────────────────────────────┘
```

Device Gateway 与 Desktop transport 共处一个 Host 进程，但使用不同监听边界和协议；这不是两套
Runtime Host，也不产生第二份 Session、Run 或 Conversation 状态。

## 3. 权威状态与职责边界

### 3.1 ez-assistant Runtime

唯一持有：

- Session、Run、Conversation 和消息顺序。
- 输入幂等、队列、取消、恢复和重试。
- 工具调用、审批、权限、记忆和 Workspace。
- 规范事件序号、快照和持久化。

终端和语音接入层都不能复制或推断这些事实。

### 3.2 Runtime Host

继续负责：

- Runtime 进程装配和本地生命周期。
- Runtime Home、实例发现和单实例所有权。
- loopback HTTP Command、SSE、上传和资源路由。
- Host/Origin/CORS、进程级 Token 和 capabilities。
- LAN Device WSS、mDNS、配对窗口、设备连接与 SpeechService 生命周期。

Device Gateway、mDNS 和 SpeechService 是 HostSupervisor 管理的可降级子系统；它们故障时应明确
报告不可用，但不能复制或替代 Runtime 的业务命令、事件和恢复接口。

### 3.3 语音接入层

当前由 Runtime Host 内的 Device Gateway 与 SpeechService 负责：

- 设备发现、配对、设备身份和连接授权。
- WSS 设备侧长连接。
- PCM 收发、缓冲、话语边界和媒体取消。
- ASR、TTS、重采样和流式分句。
- 将设备意图映射为正式 Runtime Command。
- 将 Runtime 事件和快照映射为设备状态、文本与音频。

除配对设备身份与撤销状态外，只允许持有连接和媒体状态，例如当前设备连接、话语 ID、
`client_input_id`、播放队列和输出偏好。它不能持久化第二份 Conversation，也不能成为审批和
Runtime 队列的事实源。

### 3.4 终端固件

负责：

- 驱动屏幕、麦克风、功放、按键和 Wi-Fi。
- 音频 DMA、ring buffer 和网络帧收发。
- PTT 交互、本地播放中断和状态显示。
- 连接监督、退避重连和诊断。

终端不直接持有 Runtime Token，不直接调用模型服务，也不理解工具参数、权限规则或完整会话历史。

## 4. 正式部署形态

### 4.1 当前冻结拓扑

```text
ESP32 ── mDNS + WSS /device ── Runtime Host[Device Gateway + SpeechService] ── Runtime
Desktop ── loopback HTTP/SSE ──────────────────────────────┘
```

- 终端只维持正式 Device WSS，不连接 Desktop HTTP/SSE，也不持有 Host Bearer Token。
- Device Gateway 直接调用同进程 Runtime 能力，所有设备输入进入当前 Controller Session；
  v1.0 不提供设备选择或绑定任意 Session 的能力。
- Desktop transport 与 Device Gateway 使用不同安全边界和消息契约，但共享 HostSupervisor、
  Runtime 实例和权威状态。
- 独立 Adapter 方案不再作为当前实现方向；未来只有出现明确隔离需求时才重新立项评估。

### 4.2 生命周期与故障边界

- Desktop HTTP 或 Runtime/Store 主链故障属于 Host 致命故障。
- Device Gateway、mDNS 和 SpeechService 故障属于可降级故障，不能拖垮 Runtime 主链。
- Provider 凭据和模型配置只在 Host 侧装配；终端不接触 DashScope 或其他模型服务。
- 设备撤销、连接替换、Gateway 关闭和 Provider 错误必须通过正式错误码或状态投影显式呈现。

### 4.3 S3.0 历史 Demo

S3.0 曾使用现已归档的 `agent-robot/demos/S3.0/voice-demo-host` 作为临时验证宿主：

```text
ESP32 ── Demo WebSocket + PCM ── Voice Demo Host ── ASR / agent-sdk / TTS
```

- Demo Host 位于 agent-robot，不是 ez-assistant Runtime Host、Device Gateway 或产品进程。
- Demo 可通过本地路径引用 `agent-sdk`，使用其 ephemeral 执行入口；不装配正式 Session、Run、
  Journal、审批、持久化或恢复。
- Demo 首轮允许局域网无鉴权 WebSocket，连接后直接通信；该选择仅减少原型变量，不进入正式
  安全架构，也不改变 §12 的 WSS、配对和可撤销身份要求。
- Demo 控制消息和裸 PCM 帧是私有实验格式，不进入 `assistant-protocol`，不承诺与正式设备协议兼容。
- Demo 取得的音频格式、帧长、缓冲、延迟、取消和故障证据作为后续联合版本设计的输入。

## 5. 跨项目版本基线

当前联调输入为：

- ez-assistant：v0.21.0 联调工作树，基础 HEAD `444168730d6693e9b2911e83bbcdffd957e115f7`；
  该工作树尚有未提交改动，正式组合版本必须替换为可复现提交。
- 设备协议：v1.0；Rust Host 与 Node 模拟器共享 `device-protocol-v1` fixtures。
- 正式参考终端：`tools/device-simulator` 的 Node/H5 实现及 M7 端到端测试。
- Session 语义：所有设备输入进入当前 Controller Session；设备不能指定任意 Session。
- 输出语义：Runtime `speak` 工具经 SpeechService 进入设备 FIFO，`playback_cancel` 只取消播放，
  不隐式取消 Runtime Run。

联合版本确认后，本项目需要记录：

| 项 | 记录内容 |
|---|---|
| ez-assistant 版本 | 发布 tag 与精确提交 |
| assistant-protocol | Runtime/Desktop 协议版本及必要 capabilities |
| device protocol | 主/次版本、fixtures 和兼容规则 |
| Device Gateway / SpeechService | Host 配置、Provider 与可降级状态 |
| 固件版本 | 支持的设备协议主/次版本 |
| 组合验收 | 已验证的版本组合、日期和限制 |

协议在联调期暂时冻结但不是不可修改。任何 wire 改动必须先在 ez-assistant 更新 Rust/Node codec、
fixtures 和 E2E，再同步本仓固件与组合记录；不能只修改某一端后继续沿用 v1.0 名称。

## 6. 终端固件分层

```text
Application Controller
├── Interaction State Machine
├── Audio Session Coordinator
├── Connection Supervisor
└── UI Presenter
        │
Device Protocol
├── Control Message Codec
├── Binary Audio Framing
└── Version / Capability Negotiation
        │
Services
├── Audio Capture / Playback
├── Wake Word / AFE
├── Network / WSS
├── Configuration
└── Diagnostics
        │
Drivers / BSP
├── I2S microphone
├── I2S amplifier
├── SPI display + LVGL port
├── GPIO buttons
└── Wi-Fi / NVS
```

约束：

- GPIO 和板卡差异只存在于 BSP/board config。
- 驱动不直接发送协议消息，网络层不直接操作硬件。
- 状态变更通过有界事件队列传递，音频数据使用预分配 ring buffer。
- UI 只展示 Controller 提供的投影，不推断网络或 Runtime 状态。

## 7. 终端任务与数据流

建议的 FreeRTOS 任务边界：

| 任务 | 职责 | 约束 |
|---|---|---|
| Audio Capture | I2S RX、格式转换、写入上行 ring buffer | 不等待网络 |
| Wake Word | 消费连续 PCM、运行 AFE/WakeNet、产生本地唤醒事件 | 不直接提交 Runtime；播放时可暂停 |
| Audio Playback | 从下行 ring buffer 读取并写 I2S TX | 不等待 UI |
| Network | 连接、控制消息、音频帧收发 | 不执行耗时音频转换 |
| Controller | PTT、状态机、超时和取消协调 | 单一状态写入者 |
| UI | LVGL tick、渲染和动画 | 低于音频关键路径优先级 |
| Diagnostics | 指标和有界日志 | 不记录完整语音内容 |

数据流：

```text
I2S RX → Capture Ring ─┬→ WakeNet → Controller
                       └→ Network TX
Network RX → Playback Ring → I2S TX
Buttons / Wake / Network Events → Controller → UI Projection
```

任何网络抖动都不能直接阻塞 I2S DMA；缓冲溢出或欠载必须计数并进入可诊断状态。

## 8. 设备媒体协议

device protocol v1.0 只服务硬件终端与 Runtime Host Device Gateway，不复刻 assistant-protocol。
正式跨语言真值来自 ez-assistant 的 Rust/Node codec 与 `docs/resources/device-protocol-v1/fixtures/`。

### 8.1 连接协商

- 使用 mDNS `_ez-assistant._tcp.local` 发现服务，正式路径为 WSS `/device`。
- JSON envelope 固定包含 `protocol_major`、`protocol_minor`、`message_id`、`type` 和 `payload`；
  控制消息最大 64 KiB。
- v1.0 的 `protocol_major=1`、`protocol_minor=0`；主版本不兼容时拒绝连接。
- `hello` 声明设备 capabilities 与 `output_preference`，Host 返回实际生效能力。
- 已配对设备使用稳定 `device_id` 和 Ed25519 身份完成认证；每次连接使用新的 nonce 与
  `connection_id`，连接内的媒体流 ID 不跨连接复用。

### 8.2 控制消息

v1.0 的主要语义：

- `pairing_hello` / `pairing_bind` / `pairing_confirm` / `pairing_commit`。
- `hello` 与认证结果。
- `text_input` / `listen_start` / `listen_stop` / `input_accepted`。
- `set_output_preference` / `output_preference_changed`。
- `transcript` / `text_output` / `state_changed`。
- `playback_start` / `playback_end` / `playback_cancel`。
- `ping` / `pong` / `error`。

输入使用客户端生成并可在重试时复用的 `client_input_id`；播放使用 `audio_stream_id`。设备不能通过
协议选择任意 Runtime Session，也不能从停止扬声器推断 Runtime Run 已取消。

### 8.3 音频帧

- PCM16 little-endian、16 kHz、mono，固定 20 ms / 640 bytes payload。
- 每个 WebSocket 二进制消息为 16-byte header + 640-byte payload。
- header：`version:u8`、`direction:u8`、`flags:u16be`、`stream_id:u32be`、
  `sequence:u32be`、`payload_len:u16be`、`reserved:u16be`。
- `version=1`，上行 `direction=1`，下行 `direction=2`；v1.0 的 flags 与 reserved 必须为 0。
- 控制消息声明流开始、格式、实际 sample count 和结束原因；连接断开不能表示正常音频结束。
- 非法长度、方向、序号、保留位或复用的客户端输入必须产生明确协议错误，不能静默容错。

S3.0 Demo 的裸 PCM 与私有控制消息已经冻结归档；S3.1 从零实现正式 codec，不保留兼容分支。

## 9. 交互状态机

```text
disconnected ── connected ──▶ idle
      ▲                        │ PTT down / local wake
      │                        ▼
      └──── connection lost ─ listening
                               │ PTT up
                               ▼
                           processing ── reply ready ──▶ speaking ──▶ idle
                               │
                               ├── approval ──▶ needs_attention
                               │                    │ desktop resolves
                               │                    └──────▶ processing
                               └── failure ──▶ error ── recover ──▶ idle/disconnected
```

补充规则：

- `listening` 由终端 PTT 或阶段二已验证的本地唤醒事实驱动，不依赖云端 VAD；阶段三端到端验收保留 PTT 回退。
- `processing` 包含 ASR、Runtime 和 TTS 首包等待，UI 可以展示细分诊断但不增加业务状态。
- speaking 时按 PTT：先停止本地播放并发送 `playback_cancel`，再开始新的话语；播放取消不取消
  Runtime Run。
- 审批不在终端处理，进入 `needs_attention` 并提示使用桌面端。
- 任一错误必须能回到 `idle` 或 `disconnected`，不能永久停留在 `processing`。

## 10. 输入幂等与恢复

- `client_input_id` 在一次输入开始时生成；未收到 `input_accepted` 前重连重试必须保持不变。
- `input_accepted` 是 Host 已接受该输入的正式确认，重试不能换 ID，也不能重复朗读已确认结果。
- Runtime 接受后，设备连接断开不取消 Run。
- Gateway 重启后从 Runtime/Store 权威状态恢复设备身份与当前 Controller，不用媒体缓存重建 Conversation。
- 终端重连只恢复设备状态和必要输出，不同步完整 Conversation。
- 已经开始播放的音频在设备重连后默认不自动从头朗读；是否提供显式“重播最后回复”属于后续交互。

## 11. ASR/TTS 边界

语音接入层通过内部接口装配 ASR/TTS：

```text
ASR: PCM utterance → transcript / failure
TTS: text segments → PCM stream / failure
```

- Provider 凭据、模型选择和网络日志不进入终端。
- 首版 ASR 可以整句处理；接口保留未来流式 ASR 的可能性。
- Runtime 通过 `speak` 工具显式产生可播报文本；每段最多 120 字符，每个 cycle 最多 20 段。
- SpeechService 按 FIFO 合成并下发 PCM；`playback_cancel` 清理播放，但不隐式取消 Runtime Run。
- 所有下行音频在接入层统一为终端格式。
- ASR/TTS 配置归属 Runtime Host，当前 Provider 为 DashScope；Provider 故障按可降级子系统处理。

## 12. 安全架构

- Desktop HTTP/SSE 继续只监听 loopback；Device Gateway 单独监听 LAN WSS `/device`。
- Runtime Host 的进程级 Token 不下发给 ESP32，设备链路也不使用预共享 token。
- 首次配对使用 8 位一次性配对码和 SPAKE2 P-256 双向确认；配对完成后生成 Ed25519 长期设备
  身份，设备可以在 Runtime Store 中重命名和撤销。
- 语音接入层只向已配对设备开放媒体服务，并限制并发、帧大小、话语时长和空闲时间。
- 局域网设备链路使用 WSS；在发送任何设备应用消息前，终端必须对服务端原始 DER 证书执行
  SHA-256 pin 校验。关闭系统 CA 校验本身不能构成成功路径。
- 普通日志不得记录完整语音、完整 transcript、Runtime Token 或 Provider credential。
- 终端恢复出厂设置必须删除设备侧网络和配对凭据。

## 13. 配置与版本

### 13.1 终端持久配置

只保存：

- Wi-Fi 配置。
- 语音接入层地址或发现配置。
- 设备身份与配对凭据。
- 音量、屏幕亮度和少量设备偏好。
- 固件与协议版本。

不保存模型 API Key、Runtime Token、Workspace 路径和完整对话。

### 13.2 能力协商

至少协商：

- PTT、文本显示、播放取消。
- 支持的音频格式。
- 是否支持审批提示、唤醒词、OTA 等扩展。
- 最大控制消息、音频帧和话语时长。

未协商的能力默认关闭，不能根据版本号猜测。

## 14. 可观察性与验收

### 14.1 指标

- Wi-Fi 和设备连接重连次数。
- 音频上行丢帧、下行 underrun、ring buffer 高水位。
- ASR、Runtime、TTS 首包和整体延迟。
- `client_input_id` 提交次数、幂等命中和取消次数。
- 当前状态、最后错误码和组件版本。

### 14.2 必测故障

- 监听中断网、processing 中断网、播放中断网。
- 终端、语音接入层、Runtime Host 分别重启。
- ASR 超时、TTS 超时、Runtime 拒绝和审批等待。
- 音频帧乱序、重复、过大和控制消息版本不兼容。
- 同一句重试不重复提交，不同话语不错误去重。
- Desktop 与终端同时观察同一 Session。

## 15. 冻结点

| 冻结点 | 必须确认的内容 |
|---|---|
| S1.4 | 固件分层、板卡配置、音频 API 和本地状态机 |
| S2.0 | ESP-SR/ESP-IDF 兼容组合、模型分区、Wi-Fi 凭据注入和诊断边界 |
| S2.1 | Wi-Fi Station 状态、NVS 配置、退避重连和错误分类 |
| S2.2 | WakeNet 模型、16 kHz PCM 输入适配、阈值和本地唤醒事件 |
| S2.3 | Wi-Fi、WakeNet、LVGL、PTT 与音频播放的最小功能共存边界 |
| ez-assistant v0.21 联调基线 | Host 内置 Device Gateway / SpeechService、当前 Controller Session、设备身份与生命周期 |
| S3.0 | Demo PCM 链路、ASR/TTS/Agent 可行性、延迟和缓冲证据；不冻结正式协议 |
| S3.1 | device protocol v1.0 Rust/Node/C codec、fixtures、配对、WSS、PCM 与实机故障恢复 |
| S3.2 | ASR/TTS Provider 接口、音频格式和分句策略 |
| S3.3 | Runtime 命令/事件映射、快照恢复和审批行为 |
| S3.4 | 组合版本、性能基线、故障恢复和 PCB 输入参数 |
| S4.0 | PCB 输入冻结前的长稳、资源趋势和极端故障验收 |
