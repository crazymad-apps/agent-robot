# Target network connectivity validation

This ESP32-S3 validation application exercises the production
`network_connectivity` component with credentials already stored in the `wifi_cfg`
NVS namespace. It verifies initial IP acquisition, a deliberately injected station
disconnect, the first one-second reconnect delay, automatic IP recovery, monotonic
IP epochs, and lifecycle shutdown state.

Run from `firmware/`:

```sh
./test/run_target_network_connectivity_validation.sh build
./test/run_target_network_connectivity_validation.sh flash-monitor /dev/cu.usbmodem101
```
