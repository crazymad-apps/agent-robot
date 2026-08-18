# Agent 硬件终端（agent-robot）

软硬件结合项目：**Agent 本体运行在远端**，本仓库是**终端硬件固件**（ESP32-S3），承担
监听、发声、显示、交互（UI 层 + 操作层）职责。

```
┌─────────── ESP32-S3 终端固件（本项目）───────────┐      ┌────────────────┐
│  UI 层: LVGL 小屏 / 内置 Web 页面                 │◄────►│  远端 Agent     │
│  操作层: 麦克风采集 → WS 上送 → 音频播放            │  WS  │  ASR/LLM/TTS    │
│  唤醒层: ESP-SR 离线唤醒词                         │      │  (自研协议)     │
└───────────────────────────────────────────────────┘      └────────────────┘
```

## 目录结构

```
├── docs/architecture.md       # 架构设计文档
├── main/
│   ├── app_main.c             # 入口：NVS/WiFi/各模块初始化
│   ├── ws_client.c/h          # WebSocket 客户端（连接/发送/事件）
│   ├── protocol.c/h           # 协议层（JSON 信令 + 二进制音频帧）
│   ├── audio.c/h              # 音频通路（I2S 采集/播放，当前为桩）
│   ├── ui.c/h                 # 显示与状态（当前为桩）
│   ├── Kconfig.projbuild      # menuconfig 配置项
│   └── idf_component.yml      # 组件依赖（esp_websocket_client）
└── tools/mock_agent.py        # 本地模拟远端 Agent，用于联调
```

## 快速开始

1. 安装 ESP-IDF（v5.x）：<https://docs.espressif.com/projects/esp-idf/zh_CN/latest/esp32s3/get-started/>
2. 设置目标芯片并配置：

```bash
idf.py set-target esp32s3
idf.py menuconfig
# → "Agent Terminal Configuration" 填入 WiFi 账号密码、远端 Agent 的 WS 地址
```

3. 编译烧录：

```bash
idf.py build flash monitor
```

4. 联调（可选）：本地起一个模拟远端 Agent：

```bash
pip install websockets
python3 tools/mock_agent.py   # 监听 0.0.0.0:8080/ws
```

## 与远端 Agent 的通信协议（草稿 v0.1）

传输：WebSocket，文本帧 = JSON 信令，二进制帧 = 音频数据（16kHz 16bit 单声道 PCM）。

| 方向 | type | 载荷 | 说明 |
|---|---|---|---|
| 终端→远端 | `hello` | device/sample_rate | 终端上线握手 |
| 终端→远端 | `audio_start` / `audio_data`(二进制) / `audio_end` | - | 一句话的音频流 |
| 远端→终端 | `state` | idle/listening/thinking/speaking | 状态驱动 UI |
| 远端→终端 | `text` | 对话文本 | 上屏显示 |
| 远端→终端 | `audio_start`/二进制/`audio_end` | TTS 音频 | 播放 |

> 协议以远端 Agent 实际实现为准，本仓库的 `protocol.c` 为可调整的适配层。
