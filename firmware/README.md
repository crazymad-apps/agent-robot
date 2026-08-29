# agent_robot_terminal firmware

This directory contains the production ESP32-S3 terminal firmware. It is a clean implementation of
ez-assistant device protocol v1.0 and does not depend on the archived S3.0 wire format or Host.

## Pinned build environment

- ESP-IDF: v6.0.2
- Target: ESP32-S3
- Board: ESP32-S3-DevKitC-1 v1.1 / ESP32-S3-WROOM-1-N8R8
- Flash / PSRAM: 8 MB / 8 MB octal PSRAM

Run `./idf.sh check` to verify the toolchain and naming policy, then `./idf.sh build` to build the
firmware. Local credentials and generated ESP-IDF state are ignored by Git.

Run `./test/run_host_tests.sh` for the cross-language protocol and crypto fixtures. The host test
toolchain requires a C11 compiler, `jq`, `pkg-config`, and Mbed TLS `mbedcrypto` development files
(Homebrew package `mbedtls` on macOS). The release firmware continues to use the mbedTLS version
bundled with the pinned ESP-IDF toolchain.

Run `./tools/verify_upstream_baseline.sh` before protocol work to verify both copied fixtures and the
exact ez-assistant integration files recorded by the compatibility manifest.

Run `./test/run_target_transport_validation.sh build` to build the target-only discovery/TLS/WSS
validation image. With the ez-assistant Device Gateway running and the board's `wifi_cfg` NVS
credentials provisioned, use `./test/run_target_transport_validation.sh flash-monitor <serial-port>`
to exercise the production transport components on the ESP32-S3.

The production component boundaries are defined in
[`components/README.md`](components/README.md). Components are added only when their formal API and
tests enter the implementation; archived Demo source is not copied into this tree.
