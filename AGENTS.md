# agent-robot — Agent 硬件终端（开发阶段）

软硬件结合项目：**Agent 本体与权威业务状态运行在 ez-assistant Runtime**，本终端只承担
设备交互和媒体 I/O（收音 / 播放 / 显示 / 按键 / 网络连接）。

阶段一硬件原型、S2.0–S2.3、S3.0 Demo 链路验证与 **S3.1 正式设备接入基线**均已完成，
当前阶段为 **S3.2 整体架构 Review、媒体与 Provider 收敛（开发中）**。device protocol 1.0 已冻结为
S3.1 组合快照；
ez-assistant v0.21.0 的可复现发布提交由其 M8 归档补齐。
`docs/roadmap.md` 是当前执行主线；硬件和软件实现必须遵循对应架构基线。

## 正式实现约束

- S3.1 起的 `firmware/` 是正式产品代码，不是 Demo、PoC 或过渡实现。
- 生产目录、组件、类型、函数、变量、配置键和日志事件必须使用稳定的领域名称，不得使用
  `demo`、`temporary`、`quick`、`hack`、`mock`、`fake`、`test_mode` 或阶段编号表达正式职责。
- `mock`、`fake`、fixture fault 和测试专用入口只能存在于明确的测试目录或测试编译条件中，不能进入
  release 控制流。
- 不为 S3.0 私有协议、旧 Demo Host 或历史消息保留兼容层；需要演进时按正式 device protocol 和
  联合版本流程处理。
- 正式模块以职责命名，例如 `device_connection`、`pairing_session`、`device_identity`、
  `utterance_cache` 和 `interaction_controller`；不得用阶段名或实现顺序替代领域含义。

## 文档索引

| 文档 | 内容 |
|---|---|
| [`docs/操作说明.md`](docs/操作说明.md) | **操作入口**：配对、手动重置、ASR/TTS 配置、日常对话与连接排查 |
| [`docs/通信协议.md`](docs/通信协议.md) | **第三方终端接入**：Device Protocol 1.0、配对认证、控制消息、PCM 与恢复规则 |
| [`docs/协议密码学附录.md`](docs/协议密码学附录.md) | SPAKE2、Ed25519、transcript 与 MAC 的逐字节编码约定 |
| [`docs/roadmap.md`](docs/roadmap.md) | **当前执行主线**：四阶段实施路线、阶段门与外壳旁支 |
| [`docs/hardware-architecture.md`](docs/hardware-architecture.md) | **硬件架构基线**：硬件边界、总线、GPIO、电源、PCB 迁移与冻结点 |
| [`docs/software-architecture.md`](docs/software-architecture.md) | **软件架构基线**：三方职责、语音接入层、固件、设备协议与联合版本规划 |
| [`docs/versions/README.md`](docs/versions/README.md) | **阶段索引**：各阶段状态、目录约定与当前阶段入口 |

## 当前路线摘要

- **阶段一**：ESP32-S3 开发板 + 分体模块，完成点屏、收音、放音、按键和飞线整机验证。
- **阶段二**：在飞线整机上验证 2.4 GHz Wi-Fi、断线恢复、ESP-SR/WakeNet 本地离线唤醒
  以及联网与持续收音的资源共存；不接入 Agent。
- **阶段三**：S3.0 已用隔离 Demo 验证通信、ASR/TTS 与 Agent SDK；S3.1 已完成正式 Device Gateway、
  安全协议、Runtime 端到端语音对话、连续输入聚合与播报抢占；S3.2 先 Review 整体正式架构并收敛
  媒体与 Provider，S3.3
  实现简洁屏幕 UI、滚轮和确认/返回键，S3.4 接回空闲态 WakeNet 并完成双入口与恢复矩阵。
- **阶段四**：S4.0 以最终屏幕、控件和声学布局冻结 PCB 输入，不沿用 DevKitC 外形；随后完成
  PCB v0.1、板级 Bring-up、外壳与整机验收，只有必须改板时才启动 S4.4 PCB v0.2。
- **外壳旁支**：示意模型只积累外观和人机工学约束；S4.0 冻结屏幕、PCB 与实物尺寸后再正式定型。
- **阶段五延后路线**：核心整机完成后再分别评估产品化基础、电池电源、声学增强和传输优化，
  不作为当前四阶段完成门。
