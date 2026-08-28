# ESP32-S3 协议密码学验证工程

该工程是独立于正式固件入口的目标端验证程序，直接链接 `components/device_crypto` 与
`components/device_protocol`，并在构建时嵌入冻结的三份 device protocol v1 夹具。

验证范围：

- strict control envelope 与 PCM frame 的正反例；
- 配对关联数据与认证转录的字节级一致性；
- Ed25519 公钥派生、签名、验签及篡改拒绝；
- SPAKE2 P-256 Party A 的 share、确认值和会话绑定 MAC；
- ESP32 硬件随机源的可用性、非全零与连续样本差异。

构建：

```bash
./test/run_target_crypto_validation.sh build
```

开发板连接后查看端口并烧录：

```bash
./test/run_target_crypto_validation.sh ports
./test/run_target_crypto_validation.sh flash-monitor /dev/cu.usbmodemXXXX
```

串口出现 `target protocol cryptography validation passed` 才表示目标端阶段门通过。
日志不会输出配对码、密钥、随机样本或转录内容。
