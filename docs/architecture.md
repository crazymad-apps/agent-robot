# Agent 硬件终端（agent-robot）架构总览

软硬件结合项目：**Agent 本体运行在远端**（ez-assistant，开发中），本终端硬件承担
**UI 层 + 操作层**（监听 / 发声 / 显示 / 交互）。

## 两层划分

```
┌─────────────────────────────────────────────────────────┐
│  硬件层（元器件选型）→ docs/hardware.md                  │
│  ESP32-S3-WROOM-1-N16R8 + INMP441 + MAX98357A + ST7789  │
│  飞线验证 → 自绘 PCB → 3D 打印外壳（学习型 DIY）          │
├─────────────────────────────────────────────────────────┤
│  软件层（Agent 通信、交互）→ docs/software.md（初稿）      │
│  终端 = Runtime 的远端语音 UI 客户端                     │
│  Runtime Host ← 网关 → WebSocket → ESP32                 │
│  协议语义以 assistant-protocol 为准                      │
└─────────────────────────────────────────────────────────┘
```

## 关键决策

| 项 | 决策 |
|---|---|
| 硬件路线 | 学习型 DIY，不买集成开发板 |
| 主控 | ESP32-S3-WROOM-1-N16R8（16MB Flash + 8MB PSRAM） |
| 外设 | INMP441 麦 / MAX98357A 功放 / ST7789 屏 / 按键 |
| Agent 本体 | ez-assistant（Rust，`~/github/ez-assistant`，开发中） |
| 通信 | 以 assistant-protocol 语义为准；终端↔网关走 WebSocket |
| 能力边界 | 窄接口：仅语音对话链路（唤醒/录音/播放/显示） |
| 部署形态 | 本地起步（Runtime + 网关在 PC，终端连局域网） |

> 软件层多项为「待确认」推荐方向，详见 software.md §7，随本体演进持续修改。

## 扩展方向（暂缓）

- 移动底盘（两轮差速）+ 摄像头（独立图传板）：曾讨论，**暂缓**，
  先落地基础语音终端（L0~L6）再评估。

## 文档导航

- [`docs/hardware.md`](hardware.md) — 硬件部分：元器件选型、引脚、电源、外观
- [`docs/software.md`](software.md) — 软件部分：Agent 通信、交互（初稿）
