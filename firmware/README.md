# agent_robot_terminal firmware

This directory contains the production ESP32-S3 terminal firmware. It is a clean implementation of
ez-assistant device protocol v1.0 and does not depend on the archived S3.0 wire format or Host.

For pairing, manual pairing reset, ASR/TTS configuration and connection troubleshooting, see the
[terminal operating guide](../docs/操作说明.md).

Third-party terminals can follow the [Device Protocol 1.0 integration guide](../docs/通信协议.md)
and its [cryptographic encoding appendix](../docs/协议密码学附录.md), using the shared fixtures for validation.

## Pinned build environment

- ESP-IDF: v6.0.2
- Target: ESP32-S3
- Board: ESP32-S3-DevKitC-1 v1.1 / ESP32-S3-WROOM-1-N8R8
- Flash / PSRAM: 8 MB / 8 MB octal PSRAM

Run `./idf.sh check` to verify the toolchain and naming policy, then `./idf.sh build` to build the
firmware. Local credentials and generated ESP-IDF state are ignored by Git.

The production baseline uses size optimization (`CONFIG_COMPILER_OPTIMIZATION_SIZE`, `-Os`) and
keeps assertions enabled. Existing generated `sdkconfig` files can override defaults: check the
effective compiler setting before acceptance; use `./idf.sh menuconfig` to select size optimization
when migrating an older local configuration. Debug optimization is not release-build evidence.

Run `./test/run_host_tests.sh` for the cross-language protocol and crypto fixtures. The host test
toolchain requires a C11 compiler, `jq`, `pkg-config`, and Mbed TLS `mbedcrypto` development files
(Homebrew package `mbedtls` on macOS). The release firmware continues to use the mbedTLS version
bundled with the pinned ESP-IDF toolchain.

Run `./tools/verify_upstream_baseline.sh` before protocol work to verify both copied fixtures and the
exact ez-assistant integration files recorded by the compatibility manifest.
This checks the historical S3.1 combination, not just the wire contract. Internal implementation
changes will fail its source hashes; do not rewrite that snapshot to hide a newer combination.

`python3 tools/media_diagnostics.py --help` describes the allowlisted metadata collector for
serial, foreground Host output or an existing log. It creates a new private directory with
`events.jsonl` and `summary.json`, rejects existing destinations and never saves raw lines.
Serial mode needs the ESP-IDF Python environment (pyserial). The collector sends no reset command,
but opening or reopening the USB serial port can still reset the board on the observed macOS setup;
stop any other serial monitor first. Snapshots and counters are not physical playback acknowledgements.

Run `./test/run_target_transport_validation.sh build` to build the target-only discovery/TLS/WSS
validation image. With the ez-assistant Device Gateway running and the board's `wifi_cfg` NVS
credentials provisioned, use `./test/run_target_transport_validation.sh flash-monitor <serial-port>`
to exercise the production transport components on the ESP32-S3.

The transport fault matrix uses two terminals. Start the ephemeral TLS/mDNS Host with
`./test/run_transport_fault_host.sh`, then run
`./test/run_target_transport_fault_validation.sh flash-monitor <serial-port>`. The Host injects bad
Upgrade and frame conditions while the ESP32-S3 image validates the production transport response.
Its generated certificate and private key remain in a temporary directory and are deleted when the
Host exits.

The production component boundaries are defined in
[`components/README.md`](components/README.md). Components are added only when their formal API and
tests enter the implementation; archived Demo source is not copied into this tree.
