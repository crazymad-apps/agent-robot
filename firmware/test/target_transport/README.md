# Target transport validation

This ESP-IDF project links the production `network_connectivity`, `host_discovery`, `wss_transport`,
`device_protocol`, and `device_connection` components. It uses the Wi-Fi credentials already stored
in the board's `wifi_cfg` NVS namespace and validates against a running ez-assistant Device Gateway.
The validation application does not implement a second Wi-Fi lifecycle:

- compatible mDNS discovery and deterministic paired-Host selection;
- rejection of an incorrect leaf-certificate SHA-256 pin before WebSocket Upgrade;
- successful pinned TLS and `/device` RFC 6455 Upgrade;
- receipt and strict decoding of the formal `auth_challenge` envelope.

It is a separate target-only validation image and is not linked into the production firmware.
