# ESP32-S3 入门 Demo

这是面向当前开发板 **ESP32-S3-DevKitC-1 v1.1 / N8R8** 的最小完整 ESP-IDF 工程。它会：

1. 通过串口打印 ESP-IDF、芯片、Flash、内部内存和 PSRAM 信息；
2. 初始化板载 GPIO38 地址灯；
3. 让地址灯以低亮度循环显示红、绿、蓝、熄灭，并同步输出日志。

这个工程只使用板载器件，不需要先连接屏幕、麦克风或功放。

## 文件是怎么配合的

| 文件 | 作用 |
|---|---|
| `CMakeLists.txt` | 定义整个 ESP-IDF 工程和工程名 |
| `main/CMakeLists.txt` | 告诉构建系统编译 `main.c` |
| `main/main.c` | 固件入口和 RGB 演示逻辑 |
| `main/board_config.h` | 集中保存开发板型号、Demo 版本和板载 GPIO 定义 |
| `main/idf_component.yml` | 声明官方 `led_strip` 组件依赖 |
| `sdkconfig.defaults` | 固定 N8R8 的 8 MB Flash、8 MB 八线 PSRAM 等默认配置 |
| `idf.sh` | 把常用 EIM/ESP-IDF 命令整理成短命令 |
| `test.sh` | 执行脚本语法检查、构建和产物检查，不会烧录开发板 |

ESP-IDF 中的程序入口不是桌面 C 程序常见的 `main()`，而是 `app_main()`。FreeRTOS 启动完成后会调用它。

## 第一次构建

在本目录执行：

```bash
./idf.sh check
./test.sh
```

第一次构建会自动执行 `idf.py set-target esp32s3`，并下载被固定为 3.0.3 的官方 `led_strip` 组件。构建链路可以理解为：

```text
idf.sh → EIM v6.0.2 环境 → idf.py → CMake → Ninja → 编译器 → .bin 固件
```

## 找到串口并烧录

建议第一次使用开发板上标为 **UART** 的 USB 口。连接后运行：

```bash
./idf.sh ports
```

记下新出现的 `/dev/cu.*` 设备，然后执行，例如：

```bash
./idf.sh flash-monitor /dev/cu.usbserial-110
```

串口名以你电脑实际输出为准。烧录完成后应看到芯片信息和 `RGB state` 日志，同时板载 RGB 灯循环变色。退出监视器按 `Ctrl+]`。

如果自动烧录无法让板子进入下载模式，可按住 `BOOT`，短按一次 `RST`，再松开 `BOOT` 后重试。

## 常用命令

```bash
./idf.sh build                 # 编译
./idf.sh size                  # 查看各组件占用空间
./idf.sh menuconfig            # 打开 ESP-IDF 配置界面
./idf.sh monitor <串口>        # 只看日志
./idf.sh flash <串口>          # 只烧录
./idf.sh fullclean             # 删除构建缓存，不删除源码
```

`build/`、`sdkconfig` 和下载的托管组件是本机构建产物，已忽略；`sdkconfig.defaults` 和依赖锁文件应纳入版本管理，以保证其他人能复现配置。

## 推荐的学习顺序

1. 修改 `BLINK_INTERVAL_MS`，重新构建和烧录，观察闪烁速度。
2. 修改 `colors` 数组里的 RGB 数值，理解结构体、数组和循环。
3. 给日志增加循环计数器，练习变量和 `ESP_LOGI`。
4. 运行 `./idf.sh menuconfig`，只浏览配置并搜索 `SPIRAM`，理解 `sdkconfig` 的来源。
5. 熟悉后再新增按键 GPIO 输入，避免一开始同时排查多块外设。

注意：GPIO38 接的是地址灯，不能用普通 `gpio_set_level()` 直接得到稳定颜色；代码通过 RMT 外设发送 WS2812/SK68XX 时序。
