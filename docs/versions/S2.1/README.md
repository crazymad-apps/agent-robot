# S2.1：Wi-Fi 连接与恢复

| 项 | 内容 |
|---|---|
| 状态 | 已完成：构建、烧录、凭据管理、冷启动恢复、AP 退避重连及 PTT/LVGL 共存均通过 |
| 开始日期 | 2026-08-22 |
| 所属路线 | 阶段二：联网与本地唤醒验证 |
| 前置阶段 | S2.0：已完成 |
| 下一阶段 | S2.2：WakeNet 离线唤醒（执行中） |

## 本阶段目标

- 通过串口在设备本地一次性录入 2.4 GHz Wi-Fi 凭据，并保存到 `wifi_cfg` NVS namespace。
- Station 模式通过 DHCP 获取地址，屏幕显示配置、连接、已连接、重试或错误状态。
- AP 暂时不可用时按 1/2/4/8/16/30 秒上限退避，不高速循环重连。
- Wi-Fi 故障不阻塞现有 PTT、5 秒录音、本地回放和 LVGL 状态页。

## 当前产物

- 固件版本：`S2.1-wifi.1`。
- 串口命令：`wifi set`、`wifi status`、`wifi clear`。
- 构建尺寸：`0x10f2c0` bytes，2 MiB app 分区余 `0xf0d40` bytes（47%）。
- USB Serial/JTAG 是交互式主控制台；实机 `wifi status` 输入与响应已通过。
- `srmodels.bin` 已随固件构建，但本阶段尚不运行 WakeNet。

## 安全边界

- 不在聊天、源码、`sdkconfig`、构建命令、截图或普通日志中提供真实密码。
- `wifi set` 分两行读取 SSID 和密码；设备不主动回显密码，只记录 SSID 的 16-bit 本地诊断标识。
- 当前飞线原型的 NVS 未启用加密，凭据只适用于物理受控的开发板；量产前必须重新评估 NVS/Flash 加密。
- `wifi clear` 只清除 `wifi_cfg` namespace；不会擦除整片 Flash。

## 入口

- [`阶段计划.md`](阶段计划.md)
- [`验收记录.md`](验收记录.md)
- [`../../../demos/S2.3/main/wifi_manager.c`](../../../demos/S2.3/main/wifi_manager.c)

## 阶段结论

- 2.4 GHz Station 首次连接、DHCP、NVS 冷启动恢复均通过。
- AP 消失时退避为 1/2/4/8/16/30 秒并封顶，AP 恢复后可自动重新认证并取得地址。
- 正常联网和断线重试期间，PTT 提示音、5 秒录音回放及屏幕均保持正常。
- `wifi clear` 可只清除设备 Wi-Fi 配置并回到 `WIFI SETUP`；重新配网后恢复连接。
- S2.1 阶段门通过，允许进入 S2.2；WakeNet 尚未在本阶段运行。
