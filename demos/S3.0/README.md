# S3.0 WebSocket 媒体 Demo 固件

这是从零建立、独立于 S1/S2 历史实现的新工程，只验证当前主链路：

```text
PTT 按住录音 → PCM16/16k/mono → 无鉴权 WebSocket → Voice Demo Host
PTT 松开停止 ← 原始电平播放 TTS PCM16/16k/mono ← Voice Demo Host
```

不包含 WakeNet、LVGL、旧本地录音回放或任何兼容分支；当前 Host 已接入 DashScope
ASR/TTS，Runtime 对话接入留待后续迭代。
S2.3 已验收工程整体冻结在 [`../S2.3/`](../S2.3/)。S3.0 本身也已冻结归档，本目录不再作为
后续固件的兼容或演进基线。

## 本地配置

Wi-Fi 直接读取 S2 阶段保存在 NVS `wifi_cfg` 中的凭据；应用烧录不会清空 NVS。Host 地址通过
`menuconfig → Agent Robot S3 Demo → Voice Demo Host WebSocket URI` 配置，例如：

```text
ws://192.168.31.241:8787/ws
```

该地址写入被忽略的 `sdkconfig`。协议没有 token，不能把凭据附加到 URI。

## 构建和运行

```bash
./idf.sh menuconfig
./test.sh
./idf.sh ports
./idf.sh flash-monitor /dev/cu.usbmodem1101
```

先在 PC 启动归档的 [`voice-demo-host/`](voice-demo-host/)；终端连接成功后板载 RGB 为蓝色。
按住 GPIO21 按键说话，松开后 RGB 依次变为紫色和橙色，并播放 Host 返回的 TTS PCM。
固件不再施加旧录音回放使用的 4 倍数字增益。当前将云端 TTS 原始电平衰减到 75% 后输出，
避免饱和削波并降低近距离试听音量。

## RGB 状态

- 暗蓝：连接 Wi-Fi 或 Host。
- 蓝：空闲，可按 PTT。
- 绿：录音。
- 紫：上传并等待回传。
- 橙：播放。
- 红：当前轮失败。
