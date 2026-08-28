# S2.3：联网、唤醒与媒体共存

| 项 | 内容 |
|---|---|
| 状态 | 已完成：`S2.3-coexist.1` 已通过正常联网、离线/重连和恢复后三状态最小共存冒烟测试 |
| 开始日期 | 2026-08-22 |
| 所属路线 | 阶段二：联网与本地唤醒验证 |
| 前置阶段 | S2.2：已完成 |
| 下一阶段 | S3.0：Demo 链路验证 |

## 本阶段目标

- 同时运行 Wi-Fi、WakeNet、I2S RX、ST7789/LVGL、PTT 和本地播放。
- 继续使用半双工：播放时暂停 WakeNet，结束后恢复监听。
- 记录应用尺寸、内部堆/PSRAM 快照，复用已有的音频错误、Wi-Fi 重连和唤醒日志。
- 用正常联网、AP 断开/重连中、AP 恢复后三个状态各一次本地闭环，验证最小共存可行性。

## 范围边界

- 不改变阶段一已验证接线。
- 不接入 Agent、ASR、TTS、Runtime 或设备 WSS 协议。
- 不实现播放中唤醒、AEC、双麦、OTA、电池或低功耗。
- 不在本阶段做 60 分钟长稳、CPU/任务级性能分析、资源趋势或极端压力测试；这些延期到 S4.0 PCB 输入冻结前。

## 入口

- [`阶段计划.md`](阶段计划.md)
- [`验收记录.md`](验收记录.md)
- [`../../../demos/S2.3/main/main.c`](../../../demos/S2.3/main/main.c)
- [`../../../demos/S2.3/main/wakenet_listener.c`](../../../demos/S2.3/main/wakenet_listener.c)
- [`../../../demos/S2.3/main/wifi_manager.c`](../../../demos/S2.3/main/wifi_manager.c)

## 当前产物

- 固件版本：`S2.3-coexist.1`。
- app 大小：`0x16bec0` / `0x200000`，余 `0x94140`（29%）。
- 启动和每次本地闭环后输出内部堆/PSRAM free、minimum 与 largest block 快照。
- 三个网络状态下 WakeNet/PTT 各一轮均通过；恢复联网后内部堆回到约 104 KB，PSRAM 快照不变。
