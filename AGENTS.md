# agent-robot — Agent 硬件终端（方案阶段）

软硬件结合项目：**Agent 本体与权威业务状态运行在 ez-assistant Runtime**，本终端只承担
设备交互和媒体 I/O（收音 / 播放 / 显示 / 按键 / 网络连接）。

阶段一硬件原型、S2.0–S2.3 与 S3.0 Demo 链路验证已完成，当前为 **S3.1 正式协议联调**。
ez-assistant v0.21.0 联调工作树中的 Device Gateway、SpeechService 与 device protocol v1.0
暂时作为冻结基线；联调发现必要缺口时可以同步修改 ez-assistant 协议与实现，但必须保持两仓文档、
fixtures 和组合版本一致。
`docs/roadmap.md` 是当前执行主线；硬件和软件实现必须遵循对应架构基线。

## 文档索引

| 文档 | 内容 |
|---|---|
| [`docs/roadmap.md`](docs/roadmap.md) | **当前执行主线**：四阶段实施路线、阶段门与外壳旁支 |
| [`docs/hardware-architecture.md`](docs/hardware-architecture.md) | **硬件架构基线**：硬件边界、总线、GPIO、电源、PCB 迁移与冻结点 |
| [`docs/software-architecture.md`](docs/software-architecture.md) | **软件架构基线**：三方职责、语音接入层、固件、设备协议与联合版本规划 |
| [`docs/versions/README.md`](docs/versions/README.md) | **阶段索引**：各阶段状态、目录约定与当前阶段入口 |

## 当前路线摘要

- **阶段一**：ESP32-S3 开发板 + 分体模块，完成点屏、收音、放音、按键和飞线整机验证。
- **阶段二**：在飞线整机上验证 2.4 GHz Wi-Fi、断线恢复、ESP-SR/WakeNet 本地离线唤醒
  以及联网与持续收音的资源共存；不接入 Agent。
- **阶段三**：S3.0 已用隔离 Demo 验证通信、ASR/TTS 与 Agent SDK；S3.1 直接接入 ez-assistant
  Runtime Host 内置 Device Gateway 的正式 WSS 和配对协议，再完成 Runtime 端到端语音对话；
  不复制第二套 Host。
- **阶段四**：在接口稳定后自绘 PCB、打样焊接、板级验证，并与外壳支线汇合完成整机。
- **外壳旁支**：从阶段一开始积累外观与机械约束，PCB 外形冻结后再正式定型。
