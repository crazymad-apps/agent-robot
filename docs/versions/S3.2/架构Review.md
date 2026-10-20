# S3.2 整体架构 Review

> 状态：实现级检查与 P2/P3 开发完成，P4 诊断及自动故障矩阵通过；进入真实 20 轮人工验收。
> 自动化和候选烧录不代表实听通过，S3.2 尚未封版。

## 1. Review 目标

确认 S3.1 建立的正式设备链路能够在不复制 Runtime 权威状态、不建设第二套 Host、不依赖旧 Demo
兼容的前提下，稳定承载 S3.2 的连续输入、Provider 和分段播报能力，并为 S3.3/S3.4 提供清晰接口。

## 2. 当前预期拓扑

```text
ESP32-S3 Terminal
  ├─ BSP / I2S / LVGL / PTT / Wi-Fi / NVS
  ├─ Interaction Controller / Audio / Connection Supervisor
  └─ device protocol 1.0 over LAN WSS
                         │
ez-assistant Runtime Host
  ├─ Device Gateway / identity / media session
  ├─ SpeechService / ASR / TTS / playback queue
  └─ in-process Runtime API
                         │
ez-assistant Runtime
  └─ Session / Run / Conversation / Queue / Approval / persistence
```

本图只是待核对的架构预期。Review 必须从实际装配入口追踪调用、任务、队列和状态所有者。

## 3. 结论规则

| 结论 | 含义 |
|---|---|
| 保持 | 实现与职责一致，风险和边界已有测试或可接受证据 |
| 调整 | S3.2 内必须修改；需要明确根因、所有者和回归方式 |
| 延后 | 当前风险不阻断 S3.2，并明确目标版本及不阻断依据 |

严重度使用“阻断 / 高 / 中 / 低”。“待检查”不是结论，也不能用于通过阶段门。

## 4. Review 矩阵

| ID | 维度 | 核心问题 | 实现证据 | 结论 | 严重度/处置 |
|---|---|---|---|---|---|
| AR-01 | 拓扑与生命周期 | HostSupervisor 是否只装配一套 Runtime，Gateway/SpeechService 故障是否保持可降级 | `apps/runtime-host/src/main.rs` 只构造一套 Runtime；Supervisor 将 Desktop HTTP 设为致命、Gateway/Speech 设为可降级；相关 7 项测试通过 | 调整 | 中：可降级子系统异常退出后仍可能留下过期 Ready 投影；纳入 S32-R05 |
| AR-02 | 权威状态 | Session/Run/Conversation、连接、输入聚合、播放队列和 UI 投影是否各有单一所有者 | Runtime 持有 Session/Run/Conversation；Gateway 持有连接和按设备聚合；连接持有易失播放队列；终端 UI 仅消费投影 | 保持 | 无阻断；不得把 UI、WSS 或 SpeechService 变成第二业务状态源 |
| AR-03 | 协议边界 | device protocol 是否保持窄媒体职责，是否存在设备侧推断 Runtime 事实或隐式 wire 语义 | v1.0 fixtures 三份逐字一致且 upstream baseline 校验通过；终端只处理 identity、连接、媒体和状态投影 | 保持 | device protocol 1.0 继续冻结 |
| AR-04 | 输入生命周期 | PTT down、分段 PCM、ASR、聚合窗口、Runtime submit、确认与重传是否具有闭合状态机 | 调用链已追踪；Runtime 由 Device ID + logical `client_input_id` 生成幂等键；发现 PTT 边沿丢弃和异常 stop/cancel 冻结路径 | 调整 | 高：S32-R01、R02、R03；中：R04 |
| AR-05 | 输出生命周期 | `speak`、TTS FIFO、下发、设备播放、完成、取消和迟到 PCM 是否跨层一致 | `speak` 默认串行；Host 先预留再合成；连接队列 20 段；终端 8 stream/256 frame FIFO；取消与 epoch 隔离已存在 | 调整 | 中：补齐 StartPlayback 附加确认；明确“Tool 成功=队列接纳”，不是物理播放完成 |
| AR-06 | 并发与背压 | FreeRTOS/async task、锁、channel、ring/FIFO 是否有界且不会阻塞音频关键路径 | 终端媒体队列均有界且 PCM 缓冲位于 PSRAM；Host channel 有界；SpeechService 请求 JoinSet 没有全局并发/内存预算 | 调整 | 高：PTT 可靠通道；中：Provider 请求并发预算 |
| AR-07 | 内存所有权 | PCM、transcript、TTS segment 和重传缓存的创建、转移、释放及超时是否唯一 | 终端 utterance cache 60 秒、播放 256 frame；Host 单连接上行 60 秒、播放 20 段；取消令牌负责释放，但 Host 总量未设预算 | 调整 | 中：建立 Host ASR/TTS 并发和 PCM 总量上限，记录拒绝原因 |
| AR-08 | 连接与恢复 | Wi-Fi、mDNS、WSS、identity、Gateway、Runtime、Provider 各层 epoch 和重试是否正交 | 终端 connection epoch、缓存重传和 Host 按设备聚合可跨 WSS；重连会重新启动 commit 窗口；异常 cancel 路径例外 | 调整 | 高：修复聚合永久 paused；其余恢复边界保持 |
| AR-09 | Provider 边界 | 凭据、配置、初始化、健康状态、超时、取消、降级和恢复是否由 Host 单一装配 | SpeechService 原子编译 ASR/TTS、Provider 请求有超时/取消；任务 panic 固定只降级 ASR，普通 Provider 失败不改变健康投影 | 调整 | 中：分别维护 ASR/TTS 健康和恢复，限制并发；纳入 S32-R05 |
| AR-10 | 安全与隐私 | WSS pin、设备身份、revoke、限制、日志脱敏和 credential 边界是否未被媒体逻辑绕过 | S3.1 安全 fixtures/host tests 通过；credential 只进入 Provider；完整 PCM 仅可写入 debug 目录且 release 禁用 | 保持 | 普通日志继续禁止 credential、完整 PCM 和完整 transcript |
| AR-11 | 可观察性 | 延迟、队列、输入幂等、丢弃、underrun、内存和错误是否足以定位实机问题 | 终端已有 heap/PSRAM/最大块/任务栈/rebuffer/队列错误日志；Host 缺输入聚合、Provider 延迟、队列深度和取消原因的成组诊断 | 调整 | 中：S32-R07，使用 ID/计数/耗时，不记录正文 |
| AR-12 | 资源模型 | Flash、internal heap、PSRAM、最大连续块、任务栈和峰值缓冲是否有预算与实测入口 | 终端主要 PCM 队列在 PSRAM，任务栈和容量固定；已有周期资源快照；Host 最大并发总量与实机峰值尚未验收 | 调整 | 中：S32-R06/R08；P4 记录 release size 和 20 轮趋势 |
| AR-13 | 版本与兼容 | 两仓版本、fixtures、capabilities 和协议冻结是否可复现且不存在旧 Demo 兼容分支 | agent-robot 基线提交、ez-assistant 提交及关键文件哈希已记录；fixture 校验和正式命名检查通过 | 保持 | ez-assistant 工作区正在变化，实施前必须重核相关文件哈希 |
| AR-14 | 后续接口 | S3.3 UI/控件和 S3.4 WakeNet 能否只通过 Controller/Service 接口接入，不侵入协议或 Runtime | UI 消费终端投影；PTT 进入本地交互/音频接口；Host 只接收统一 listen/text 输入；WakeNet 可复用 capture 入口 | 保持 | S3.3/S3.4 不增加 Runtime 权威状态；PTT 可靠性修复不得绑定具体菜单 |
| AR-15 | 硬件/软件边界 | 当前 GPIO、音频格式、功放控制、屏幕、供电和资源假设是否与正式固件一致，且未提前冻结 PCB 器件 | board config 集中定义 N8R8、240×240、I2S、功放和 PTT；架构文档明确滚轮/按键是 S3.3 候选、PCB 是 S4.0 冻结 | 保持 | 本阶段不改 GPIO、不冻结新器件或 PCB 尺寸 |

## 5. 调用链证据

### 5.1 上行：PTT 到 Runtime

```text
terminal_button task
  -> terminal interaction event
  -> terminal_lifecycle queue/task
  -> voice_input_begin + audio_io_capture_begin
  -> audio_io task/capture callback
  -> voice_input frame queue + PSRAM utterance cache
  -> device_connection outbound queue/task
  -> WSS listen_start + PCM frames + listen_stop
  -> Host authenticated connection loop/UplinkUtterance
  -> per-device VoiceTurnAggregation
  -> SpeechService command + owned Provider request
  -> ordered transcript merge
  -> AssistantRuntime::submit_session_input
```

身份与确认点：

- `capture_session_id` 只标识终端本地采集生命周期；`connection_epoch` 隔离断线前后数据。
- 每个物理分段有稳定 `client_input_id` 和 `stream_id`；`input_segment_accepted` 是 Host 已接管该段的
  确认，之后终端才能释放对应重传缓存。
- 同一连续输入窗口使用第一段 `client_input_id` 作为 logical ID；Runtime 将 Device ID 与 logical ID
  映射为稳定幂等键，因此 WSS 重连或最终提交重试不会创建第二条 Input。
- `input_accepted` 表示 Runtime 已可靠接纳合并后的逻辑输入，不表示 Run 已执行完成。

### 5.2 下行：speak 到设备播放

```text
Runtime serial speak tool call
  -> DeviceChannelOutputDispatcher
  -> Gateway PreparePlayback reservation
  -> SpeechService TTS request
  -> Gateway StartPlayback
  -> per-connection playback FIFO
  -> playback_start + 20 ms PCM frames + playback_end
  -> terminal device_connection task/media callback
  -> audio_io PSRAM playback FIFO
  -> audio_io task/I2S amplifier
```

- `output_id = run_id + tool_call_id`，Host 另生成 wire `stream_id`；终端再用 `connection_epoch` 隔离旧连接。
- Host 单连接最多预留 20 段；终端最多保存 8 个 stream、256 个 20 ms frame，初播和 underrun 恢复
  都要求 100 frame（2 秒）缓冲。
- `speak` 成功的正式语义是“目标播放队列已经接纳”，不是“扬声器已经物理播放完成”。Host 在最后一帧
  发出后发送 `playback_end`；终端独立完成剩余 FIFO。S3.2 不为物理播放完成新增 wire ack。
- PTT 先清空终端当前及剩余 FIFO并发送 `playback_cancel`，Host 取消连接播放队列和对应 cancellation；
  随后的首个新 logical 输入再取消旧 Runtime Run。

### 5.3 故障与恢复

- Wi-Fi/WSS：终端 supervisor 退避重连；新 `connection_epoch` 作废旧媒体；未取得 segment ack 的上行缓存
  以原身份重传。
- Host 重连：`VoiceTurnAggregation` 按 Device ID 保存在 Gateway，共享于 WSS 连接；非空聚合重启 2 秒
  commit 窗口，等待设备补发可能丢失确认的分段。
- Provider：请求有 30 秒默认/120 秒最大超时和 cancellation；配置重载原子替换新请求的 Provider，
  已在途请求使用旧快照完成或取消。S32-R05 已实现 ASR/TTS 独立健康、单侧配置隔离、请求总超时及
  ASR 4/TTS 2 的入口总预算；满载明确拒绝，后续成功请求验证恢复，旧 Provider 结果不覆盖新健康。
- 播放：断线、连接替换、revoke 或 PTT 取消易失播放，不重播历史回复；旧 stream 由 epoch/ID 检查丢弃。
- Runtime：Session/Run/Conversation 只由 Runtime/Store 恢复；Gateway 不缓存或重建业务事实。

## 6. 问题处置表

| 问题 ID | 关联 Review | 现象/风险 | 根因证据 | 所属仓库/组件 | 决策 | 目标版本 | 验证 | 协议影响 |
|---|---|---|---|---|---|---|---|---|
| S32-R01 | AR-04/06 | PTT 按下或释放在普通事件队列满时被永久丢弃，可能无法开始或结束录音 | `terminal_interaction_event()` 使用零等待 `xQueueSend`，失败后只增加总丢弃计数 | agent-robot / interaction + terminal lifecycle | 已改为由 `terminal_button` task 阻塞交付状态转换；队列未创建时保留异常计数 | S3.2 P2-M1：已完成 | Host 自动化与 release build 通过；实机队列压力验证留待 P4 | 无 |
| S32-R02 | AR-04/08 | 已有分段后取消当前空分段，logical 输入可能永久停在 paused | `listen_start` 清除 commit deadline；活动 `listen_cancel` 仅在聚合为空时清除，否则不重新 schedule | ez-assistant / Device Gateway connection | 已统一收口：空聚合清除，已有分段重新启动 commit deadline | S3.2 P2-M1：已完成 | connection 单元测试 9 项通过，包含新增聚合状态用例 | 无 |
| S32-R03 | AR-04/08 | 错误 stop 或旧 Run 取消失败后仍可能留下不一致输入状态 | `listen_stop` 在完整校验前 `receiving.take()`；`cancel_active_controller_run` 报错后未 `continue` | ez-assistant / Device Gateway connection | 已改为完整校验后转移 PCM；空段显式恢复/清除聚合；旧 Run 取消失败时拒绝启动新 logical 输入 | S3.2 P2-M1：已完成 | connection 单元测试 9 项与生产 target `cargo check` 通过 | 无 |
| S32-R04 | AR-04/06 | 分段识别包装任务没有显式 owner；异常 panic 时缺少完成事实 | `spawn_segment_recognition` 丢弃 `JoinHandle`，聚合仅由正常路径减少 pending | ez-assistant / Device Gateway | 已纳入 Gateway `TaskTracker`；取消、普通错误和 panic 均生成 `RecognitionOutcome`，Gateway 停止时取消聚合并等待受管任务 | S3.2 P2-M2：已完成 | 注入取消与 panic 后 pending 归零、取消句柄清空；Device 测试 18 项通过，Host `--no-deps` Clippy 通过 | 无 |
| S32-R05 | AR-01/07/09 | Provider 健康可能误报，跨设备并发 PCM 缺总预算 | Speech request JoinError 固定只写 ASR Degraded；普通错误不投影健康；JoinSet 无并发限制 | ez-assistant / SpeechService | 已实现独立配置/健康/恢复、ASR 4/TTS 2 准入、整次请求超时和取消回收；reload 旧结果按 Provider 身份隔离 | S3.2 P2-M3：已完成 | 语音测试 25 项、全仓测试与 Clippy 通过；真实服务体验待 P4 | 无 |
| S32-R06 | AR-05/07/12 | TTS 合成后发送 StartPlayback 只确认 command 入队，不确认仍附加到预留槽位 | connection command handler 忽略 `attach_playback_output` 的 bool | ez-assistant / Gateway playback | 实际附加后通过 oneshot 确认，PreparedPlayback guard 负责失败/超时/丢弃释放，消除 detached watcher | S3.2 P3：开发及自动化完成 | Device 28 项通过，覆盖 PTT/断线/过期槽位/重复/调用方消失/超时/短音频完成竞争；M8 E2E 通过 | 无 |
| S32-R07 | AR-11 | Host 难以区分聚合等待、Provider 慢、队列满和用户取消 | 缺统一 lifecycle 诊断字段和队列/延迟计数 | ez-assistant + agent-robot diagnostics | 已增加摘要 ID、阶段耗时、深度、高水位、取消/丢弃原因和私有目录白名单采集器 | S3.2 P4：开发及自动化完成 | 采集器 8 项、Host 脱敏 ID 测试、终端新资源快照通过；不含正文/凭据/PCM | 无 |
| S32-R08 | AR-12 | 代码容量有界，但缺当前 release 镜像与 20 轮峰值预算 | 终端有资源采样入口，S3.2 尚无实测记录 | agent-robot / resource monitor | 已固定 -Os/保留断言，记录 size、烧录、启动资源和关键栈；长稳仍待人工 | S3.2 P4：部分完成 | release app 0x1239d0、62% app 分区余量，启动后 2 份资源快照；20 轮在线趋势未执行 | 无 |
| S32-R09 | AR-05/06/11 | 高：结束消息晚于最后样本消费及 I2S 停止时，队首永远等待预缓冲，后续段不能播放 | `audio_io_task` 原先先检查 `hardware_playing`/head ready，后检查 head drained；旧代码在正式 worker 确定性回归中第二次进入空队首等待 | agent-robot / audio_io | 完成判定前移，I2S 仍有数据时继续等待排空，已停止时直接完成并推进 FIFO；补结束原因、重缓冲和进度日志 | S3.2 播报中断专项：代码完成 | 旧代码回归失败、新代码通过；全量 Host tests、release build 和烧录校验通过，真实语音待复验 | 无 |

当前阻断项为零；R01～R07 的实施及自动化完成，R08 的 20 轮在线趋势和真实实听待执行。
用户进一步授权直接推进至人工验证，P3/P4 自动工作已完成，不再等待逐里程碑确认。
S32-R09 已修复并通过自动化，但尚不能认定它就是 9 月 2 日“第二段半途停止”的实际触发原因；
真实多段实听未完成，阶段完成门保持未通过。

## 7. 冻结输入与自动化证据

### 7.1 代码快照

- agent-robot：`main` / `0384f64c7a7f18d675d1f1d7026e6b96aa988a37`。Review 期间只有本仓
  路线图和 S3.2 文档改动；6 个关键固件文件的 SHA-256 已记录在本次工作日志。
- ez-assistant：P2-M1 已随 v0.22.0 归档进入提交
  `ee3930ee1bd184fd0b5b6af08fde258cf2698dac`。P2-M2 在该提交上仅修改 Device Gateway 的
  `connection.rs` 与 `gateway.rs`；同仓另有正在进行的 v0.23 改动，
  本任务未覆盖其 protocol/runtime/storage 文件。
- agent-robot 的三份 protocol fixtures 与 ez-assistant 上游逐字相同，
  `verify_upstream_baseline.sh` 通过。

### 7.2 P1 基线测试

| 测试 | 结果 |
|---|---|
| agent-robot `firmware/test/run_host_tests.sh` | 通过：协议、crypto、identity、pairing、WSS、重连策略、utterance cache、PCM、播放 FIFO、按键策略 |
| agent-robot production naming | 通过 |
| runtime-host connection 单元测试 | 8 通过 |
| runtime-host Speech config/provider/service 单元测试 | 11 通过 |
| runtime-host Supervisor 单元测试 | 7 通过 |
| runtime-host Gateway 边界测试 | 1 通过 |
| assistant-runtime device/channel 单元测试 | 11 通过 |

这些结果取得于 P1 冻结快照，证明正常路径和主要边界存在；P2-M1/P2-M2 的新增回归、当前工作区
验证偏差和限制以 [`验收记录.md`](验收记录.md) 为准。

## 8. 决策记录

| 日期 | 决策 | 依据 | 影响 |
|---|---|---|---|
| 2026-09-01 | S3.2 先完成整体架构 Review，再实施媒体与 Provider 收敛 | S3.1 已完成正式主链，需要在继续扩展前核对实际边界 | Review 未通过前不做结构性重构 |
| 2026-09-01 | device protocol 1.0 继续冻结 | 当前没有已证实且必须改变 wire 的问题 | Review 可提出变更，但不能自动解冻 |
| 2026-09-01 | `speak` 成功定义为播放队列接纳，而非物理扬声器完成 | Runtime Tool、Host 队列和终端 FIFO 均为异步边界，当前无需依赖物理完成推进业务状态 | 不新增 playback-completed wire ack；诊断仍记录发送和终端消费 |
| 2026-09-01 | 先完成 R01～R05，再进入 TTS 队列收敛 | 输入冻结会阻塞后续所有轮次，优先级高于播放体验优化 | S3.2 P2 拆成终端控制可靠性、Host 聚合、Provider 三组回归 |
| 2026-09-01 | R01～R03 作为 P2 第一确认里程碑独立收口 | 三项均属于同一输入可靠性故障面，且不改变协议或 Runtime 业务语义 | 修复与最小验证完成后暂停，确认后再进入 R04～R05 |
| 2026-09-02 | R04 复用 Gateway 生命周期建立识别任务所有权 | ASR 聚合跨 WSS 连接存在，不能归属单连接；`TaskTracker` 已是 workspace 依赖且不新增业务状态或队列 | Gateway 启停负责取消和等待；panic 复用现有 `Unavailable` 失败语义，不改变 device protocol |
| 2026-09-05 | 按用户授权连续推进到人工验证 | P2/P3/P4 的自动化可以在隔离环境完成；不再逐里程碑暂停 | 人工 20 轮、旧 Host 重启和设备接入仍需用户操作，不擅自覆盖运行环境 |
| 2026-09-05 | 明确发布优化与历史协议快照边界 | 旧默认配置为 -Og；旧组合校验会随实现演进失败 | 当前固定 -Os/保留断言；保留 S3.1 历史快照，单独记录 S3.2 候选哈希，不弱化校验 |

## 9. Review 结论

整体职责划分可以保持，不需要重建 Host、复制 Runtime 状态或解冻 device protocol。当前问题集中在
既有内部状态机的异常收口、任务所有权和资源预算，均可在 agent-robot 与 ez-assistant 各自职责内修复。

P1 Review 门结论：**有条件通过**。允许进入 P2，但不允许跳过 S32-R01～R05 直接做体验性优化；P2
完成并回归后重新核对相关文件哈希和 ez-assistant 工作区重叠情况。

## 10. Review 完成条件

- AR-01～AR-15 已全部具有证据和正式结论。
- 阻断项为零；高优先级调整项已进入实施清单并有回归计划。
- 延后项明确目标版本和不阻断 S3.2 的依据。
- 两条主调用链、媒体内存所有权和故障恢复路径均可追踪。
- 协议维持冻结或已按联合流程完成明确变更决策，不存在单边隐式语义。
