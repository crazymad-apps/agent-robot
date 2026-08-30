# Device authentication target validation

This ESP32-S3 validation image links the formal network, discovery, identity,
authentication and Device Connection components. It uses the isolated
`dev_auth_val` NVS namespace, verifies pending credential activation after a
signed hello, then verifies that `device_revoked` clears the credential while
retaining the pinned Host binding.
