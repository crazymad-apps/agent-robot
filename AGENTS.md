# agent-robot — Agent 硬件终端（方案阶段）

软硬件结合项目：**Agent 本体运行在远端**（ez-assistant），本终端硬件承担
**UI 层 + 操作层**（监听 / 发声 / 显示 / 交互）。

当前处于**方案讨论阶段**，仓库仅保留方案文档，暂不落地开发。

## 文档索引

| 文档 | 内容 |
|---|---|
| [`docs/architecture.md`](docs/architecture.md) | 架构总览：项目定位、两层划分、关键决策 |
| [`docs/hardware.md`](docs/hardware.md) | **硬件部分**：元器件选型、引脚接线、电源、外观 3D 打印 |
| [`docs/software.md`](docs/software.md) | **软件部分**（初稿）：Agent 通信、交互设计、待确认项 |

## 方案摘要

- 学习型 DIY 路线：不买集成开发板；ESP32-S3-WROOM-1-N16R8 + 分体模块
  （INMP441 麦 / MAX98357A 功放 / ST7789 屏）飞线验证 → 自绘 PCB → 3D 打印外壳
- 终端 = ez-assistant Runtime 的远端语音 UI 客户端；协议语义以 assistant-protocol 为准，
  终端↔网关走 WebSocket，网关承载 ASR/TTS（多项待确认）
- 学习路线 L0~L6：点屏 → 收音 → 放音 → 飞线整机联调 → 画板打样焊接 → 3D 建模打印装配
