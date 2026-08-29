#ifndef NETWORK_RECONNECT_POLICY_H
#define NETWORK_RECONNECT_POLICY_H

#include <stdint.h>

uint32_t network_reconnect_policy_delay_ms(uint32_t reconnect_attempt);

#endif
