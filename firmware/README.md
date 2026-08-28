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

The production component boundaries are defined in
[`components/README.md`](components/README.md). Components are added only when their formal API and
tests enter the implementation; archived Demo source is not copied into this tree.
