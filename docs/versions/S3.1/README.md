# S3.1：正式设备接入

| 项 | 内容 |
|---|---|
| 状态 | 执行中：P0 正式工程基线已建立，上游可复现提交待冻结 |
| 设计日期 | 2026-08-28 |
| 所属路线 | 阶段三：Agent 接入 |
| 前置阶段 | S3.0：已完成并归档 |
| 后续阶段 | S3.2：媒体与 Provider 收敛 |

## 阶段目标

从空工程实现 ESP32-S3 正式终端，接入 ez-assistant Runtime Host 的 Device Gateway，完成：

1. device protocol v1.0 的 C codec 与 Rust/Node fixtures 逐字节一致。
2. mDNS 发现、TLS 证书指纹固定、WSS、SPAKE2 P-256 配对和 Ed25519 认证。
3. PTT 上行 PCM、最小状态下发、TTS PCM 播放和播放取消。
4. 通过正式 Host、当前 Controller Session 和真实 ASR/TTS 完成首轮端到端语音对话。

S3.1 只建立正式设备接入基线，不以连续 20 轮、Provider 故障矩阵或完整 Runtime 恢复作为本阶段
完成条件；这些内容分别在 S3.2～S3.4 收敛。

## 已冻结的设计结论

- S3.1 从零建立新工程，不继承 S3.0 Demo 协议，不保留兼容分支；只复用已经验证的 GPIO、音频格式
  和硬件参数。
- `firmware/` 从创建时即视为正式产品代码，不设置 Demo/PoC 生产路径，不使用临时性或阶段性命名；
  测试替身和故障注入只能存在于测试目录或测试编译条件。
- 终端只连接 Device WSS `/device`，不连接 Desktop HTTP/SSE，不持有 Runtime Token、Provider Key
  或任何预共享 token。
- 首次连接必须先校验 TLS 原始 DER 证书 SHA-256 指纹；成功后才允许发送 WebSocket Upgrade 和
  设备应用消息。
- 设备声明一组与 S3.1 实际能力一致的 capability：PCM 输入/输出、播放取消和最小状态显示；不声明
  文字输入、回复正文显示或 transcript 显示能力。
- 屏幕只承担配对码和最小联调状态，不做回复排版、动画、进度可视化或详细错误交互；完整 UI 设计
  延后到正式链路稳定后的独立版本。
- 未配对设备开机后自动扫描唯一开放配对的 Host，不用按键进入或选择配对；唯一按键保持为 PTT。
  Desktop 只打开目标 Host 的 pairing window，设备显示 6 位码后由用户在 Desktop 确认。
- 6 位码按“设备启动会话”生成并保持稳定；Host 重启、WSS 开关、mDNS 消失或 pairing window 重开
  都不换码。临时断线可恢复同一有效 pairing attempt；窗口/Host 已失效时则重建密码学临时上下文。
- Host 解除配对后，终端清除长期设备身份并自动回到未配对扫描；关闭 pairing window 只结束当前候选，
  不改变已有配对关系或长期身份。
- S3.1 全版本以 PTT 为唯一 ASR 触发入口，不编译或运行 WakeNet；自动唤醒延后到正式设备链路稳定后的
  独立版本，不能掩盖网络或协议问题。
- 整段话语在 PSRAM 保留到 `input_accepted`，断线后以同一 `client_input_id` 完整重传；确认接受后
  立即释放，不缓存或重放完整 Conversation。
- 密码学和 WSS 先设独立可行性门。未通过共享向量和实机 TLS pin 前，不展开 UI、ASR/TTS 或
  Agent 联调。

## 联调基线

- ez-assistant：v0.21.0 联调工作树；当前已知基础提交为
  `444168730d6693e9b2911e83bbcdffd957e115f7`。该工作树尚未形成可复现联合版本，开始实现前必须
  记录实际提交及 dirty 状态。
- device protocol：`1.0`。
- 协议真值：ez-assistant 的 Rust Host、Node 模拟终端及
  `docs/resources/device-protocol-v1/fixtures/`。
- ESP-IDF：继续固定当前已验证的 `v6.0.2`，不在 S3.1 同时升级工具链。
- 板卡：ESP32-S3-DevKitC-1 v1.1 / ESP32-S3-WROOM-1-N8R8。

## 文档入口

- [`方案设计.md`](方案设计.md)：架构、协议、安全、状态机、资源与关键选型。
- [`断线重连设计.md`](断线重连设计.md)：分层恢复、退避、epoch、输入幂等、错误分类与故障矩阵。
- [`阶段计划.md`](阶段计划.md)：实施顺序、阶段门、停止条件和跨仓变更流程。
- [`验收记录.md`](验收记录.md)：当前为待执行模板，只在取得实测证据后填写通过结论。

## 当前实施进度

- 已创建根目录 [`../../../firmware/`](../../../firmware/) 正式 ESP-IDF 工程，project 名为
  `agent_robot_terminal`，首次 ESP-IDF v6.0.2 构建通过。
- 已固定直接依赖和 transitive lock，同步 device protocol v1.0 三份 fixtures 及 SHA-256。
- P1 已实现 strict control envelope、256 项 message replay window、capability 序列化、长度前缀
  pairing/auth transcript、strict base64url 与 PCM frame codec；共享 fixtures 和边界测试均已通过主机测试，
  对应组件已通过 ESP32-S3 release 构建。
- 已在 ez-assistant 联调树原子修改 6 位配对码和离线 revoke，Rust/Node/Desktop/M7 验证通过。
- ez-assistant v0.21.0 尚是包含未提交文件的联调树；精确文件 hash 已记录，但 P0 的
  “由上游 commit 复现”门仍待上游正式提交后通过。
