# S2.0：联网与唤醒约束确认

| 项 | 内容 |
|---|---|
| 状态 | 已完成：兼容组合、模型分区和凭据方式已冻结（运行验证进入 S2.1/S2.2） |
| 开始日期 | 2026-08-22 |
| 所属路线 | 阶段二：联网与本地唤醒验证 |
| 前置阶段 | S1.4：已完成 |
| 下一阶段 | S2.1：Wi-Fi 连接与恢复 |

## 目标

- 确认 ESP32-S3-DevKitC-1 N8R8、当前 ESP-IDF 与 ESP-SR/WakeNet 的可构建组合。
- 在 8 MB Flash 中为应用、模型、NVS 和诊断数据规划可验证的分区布局。
- 定义不把 SSID、密码或密钥提交仓库和普通日志的 Wi-Fi 配置方式。
- 冻结内置唤醒模型、16 kHz PCM 输入适配、测试环境和阶段停止条件。

## 已知输入

- 主控：ESP32-S3-WROOM-1-N8R8，8 MB Flash、8 MB PSRAM。
- Wi-Fi：芯片原生支持 2.4 GHz 802.11b/g/n，不支持 5 GHz。
- 麦克风输入：INMP441，16 kHz、单声道、signed PCM16 已通过实机验收。
- 当前固件：`S1.4-loopback.1`，屏幕、PTT、录音和本地回放长稳已通过。
- WakeNet：已冻结 `esp-sr 2.4.7` 与官方中文 `wn9_nihaoxiaozhi_tts`（“你好小智”）；模型、模型加载、WakeNet 与 AFE API 均已在隔离副本中构建和链接通过。

## 冻结结论

- 工具链组合：ESP-IDF `v6.0.2`、`espressif/esp-sr ==2.4.7`、ESP32-S3 N8R8。
- 模型：`CONFIG_SR_WN_WN9_NIHAOXIAOZHI_TTS=y`，生成的 `srmodels.bin` 为 291,042 bytes。
- 分区：2 MiB factory app、256 KiB recording、1 MiB model；`0x350000` 之后仍有 4.6875 MiB 未分配。
- 完整 AFE/WakeNet 链接探针为 `0xd8330` bytes，2 MiB app 分区余 58%；静态 DIRAM 余 184,431 bytes。
- 凭据：S2.1 使用串口一次性录入并写入专用 NVS namespace；源码、`sdkconfig.defaults`、构建参数和普通日志均不得出现密码。
- 已知警告：ESP-SR 2.4.7 在 IDF 6.0.2 下会报告上游 Kconfig `default False` 格式提示；当前不阻断构建，但 S2.1/S2.2 必须继续观察，不将其误报为零风险兼容。

## 当前边界

- 本阶段不连接 ez-assistant，不实现 ASR/TTS、Runtime 或 WSS。
- 首先使用官方内置唤醒模型验证；自定义唤醒词不作为阶段门。
- PTT 始终保留；播放时允许暂停 WakeNet，首版不做播放中唤醒和 AEC。
- S2.0 未烧录开发板；板上仍是已通过的 `S1.4-loopback.1`。首次烧录 S2 固件前须明确提示分区表变化，并保留 S1.4 可恢复构建。

## 入口

- [`阶段计划.md`](阶段计划.md)
- [`基线与测试矩阵.md`](基线与测试矩阵.md)
- [`验收记录.md`](验收记录.md)
- [`../../../demos/S2.3`](../../../demos/S2.3)

## 官方依据

- [ESP32-S3 数据手册](https://documentation.espressif.com/esp32-s3_datasheet_en.pdf)
- [ESP32-S3-DevKitC-1 用户指南](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/)
- [ESP-SR ESP32-S3 使用指南](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/)
- [WakeNet 模型说明](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/wake_word_engine/README.html)
- [ESP-SR 模型分区说明](https://docs.espressif.com/projects/esp-sr/en/latest/esp32s3/flash_model/README.html)
