# Production component boundaries

| Component | Responsibility |
|---|---|
| `board` | Board pins, buses and electrical configuration |
| [`device_crypto`](device_crypto/) | SPAKE2, Ed25519, transcripts, HMAC/HKDF and base64url |
| `device_identity` | Pending/active device identity and pairing reset |
| [`host_discovery`](host_discovery/) | mDNS discovery and deterministic Host selection |
| [`wss_transport`](wss_transport/) | TLS certificate pinning and the required RFC 6455 subset |
| [`device_protocol`](device_protocol/) | Strict control envelopes and PCM frame codec |
| [`device_connection`](device_connection/) | Pairing, authentication, heartbeat and connection recovery |
| `audio_io` | I2S capture, playback and PCM conversion |
| `interaction` | PTT lifecycle and the single authoritative interaction state |
| `ui` | Minimal LVGL status projection |
| `diagnostics` | Bounded counters, resource snapshots and redacted events |

Release components cannot depend on test-only code. New component directories are created together
with a stable public contract and verification, not as empty placeholders.
