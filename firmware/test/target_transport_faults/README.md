# Target transport fault validation

This target-only ESP-IDF project links the production discovery, WSS transport, protocol, and
connection Supervisor components. It connects to `transport_fault_host.mjs` over Wi-Fi and validates:

- certificate rejection before any WebSocket Upgrade bytes;
- invalid Upgrade rejection;
- 64 KiB fragmented UTF-8 text with an interleaved Ping and automatic Pong;
- the exact 656-byte binary frame contract and Close echo;
- oversized text, partial-frame timeout, and masked-server-frame rejection;
- heartbeat timeout, stale epoch rejection, and recovery backoff on the target architecture.

The fault Host certificate and private key are ephemeral and are never written to the repository.
The board uses the Wi-Fi credentials already stored in its `wifi_cfg` NVS namespace.

Run `./test/run_transport_fault_host.sh` from `firmware/` in one terminal. After it prints
`transport_fault_host_ready`, run
`./test/run_target_transport_fault_validation.sh flash-monitor <serial-port>` in another terminal.
Both processes report an explicit pass line only after all six network scenarios complete.
