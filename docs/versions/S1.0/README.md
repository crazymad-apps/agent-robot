# S1.0：准备与约束确认

| 项 | 内容 |
|---|---|
| 状态 | 已完成：工具链、实机烧录/监视、板载 RGB 视觉验证及 S1.1 所需硬件/GPIO 输入均已通过 |
| 开始日期 | 2026-08-18 |
| 完成日期 | 2026-08-21 |
| 所属路线 | 阶段一：硬件组装与功能点验证 |
| 前置阶段 | 无 |
| 下一阶段 | S1.1：放音验证 |

## 阶段目标

在写入硬件驱动前，冻结一套可以复现的原型输入：

- 开发板和所有分体模块的准确型号、版本、引脚与供电要求。
- 不与启动、USB、Flash/PSRAM 和板载器件冲突的飞线 GPIO 表。
- 可复现的 ESP-IDF 安装、激活、编译、烧录和串口监视流程。
- 可供 S1.1～S1.4 持续演进的最小固件工程骨架。

## 当前结论

### 已确定

- 目标芯片：ESP32-S3。
- 目标开发板：官方 ESP32-S3-DevKitC-1 v1.1，搭载 ESP32-S3-WROOM-1-N8R8；实物已核对。
- 固件框架：原生 ESP-IDF，不采用 Arduino 框架。
- 固件主语言：C；依赖和构建使用 ESP-IDF/CMake。
- 首版供电：USB 5V，无电池。
- 首版交互：单麦、单声道、PTT 半双工。
- 工具链：ESP-IDF v6.0.2，已安装并完成 clean build 验证。
- 历史工程快照：[`../../../demos/S2.3`](../../../demos/S2.3)，包含从 S1.0 逐步演进至 S2.3 的已验收实现。
- 实机路径：原生 USB 枚举为 `/dev/cu.usbmodem1101`，已完成 flash/monitor；日志确认
  ESP32-S3 rev 0.2、8MB Flash、8MB PSRAM 和 GPIO38 状态循环。
- 候选 GPIO：已用 DevKitC-1 v1.1 官方引脚表、原理图和 ESP32-S3 GPIO 文档复核通过。

ESP-IDF 官方 stable 文档当前对应 v6.0.2，macOS 上 v6.0 及以后推荐使用 EIM 安装；
`esp-bsp` 当前兼容 ESP-IDF 6.0，可支撑后续 LVGL 组件评估：

- [ESP-IDF v6.0.2 ESP32-S3 Get Started](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html)
- [macOS EIM 安装说明](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/macos-setup.html)
- [Espressif BSP 兼容矩阵](https://github.com/espressif/esp-bsp#supported-idf-versions)

### 已完成的环境核对

| 项目 | 结果 |
|---|---|
| 主机 | Apple Silicon `arm64`，macOS 26.0 |
| 系统默认 Python | 3.14.7，`/opt/homebrew/bin/python3`；不用于本阶段 IDF 虚拟环境 |
| EIM 环境 Python | 3.13.3，已通过虚拟环境检查并被 v6.0.2 使用 |
| CMake | 4.0.1，`/opt/homebrew/bin/cmake` |
| Git | 已安装，`/opt/homebrew/bin/git` |
| EIM | 0.18.0，已安装并选中 ESP-IDF v6.0.2 |
| `idf.py` | `eim run` 环境中为 ESP-IDF v6.0.2 |
| Ninja | EIM 环境中为 1.12.1 |

以上工具版本、`esp32s3` target、最小工程 clean build、真实开发板烧录和串口监视均已完成
验收；用户已确认板载 RGB 实际按 `red / green / red / off` 循环。

### 后续阶段继续核对

- UART 桥接口的单独枚举和烧录路径；原生 USB Serial/JTAG 路径已通过。
- 显示 BLK 有效电平、麦克风芯片本体证据、喇叭额定功率、按键和辅材。
- 显示 BLK 首次点屏先接 3.3V；其 GPIO/PWM 驱动方式在 S1.2 测量背光电流后确定。

## 文档入口

- [`阶段计划.md`](阶段计划.md)：S1.0 的执行步骤与停止条件。
- [`硬件核对表.md`](硬件核对表.md)：采购、实物和电气信息核对。
- [`验收记录.md`](验收记录.md)：实际验证证据与阶段结论。
- [`../../hardware-architecture.md`](../../hardware-architecture.md)：硬件架构基线。
- [`../../roadmap.md`](../../roadmap.md)：总体实施路线。

## 当前下一步

进入 [`../S1.1`](../S1.1/)；先构建低幅度测试音固件，再按断电接线表连接功放与喇叭。
