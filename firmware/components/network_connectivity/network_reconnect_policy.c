#include "network_reconnect_policy.h"

#include <stddef.h>

uint32_t network_reconnect_policy_delay_ms(uint32_t reconnect_attempt)
{
    static const uint32_t intervals_ms[] = {
        1000U,
        2000U,
        4000U,
        8000U,
        16000U,
        30000U,
    };
    size_t index = reconnect_attempt == 0U ? 0U : reconnect_attempt - 1U;
    if (index >= sizeof(intervals_ms) / sizeof(intervals_ms[0])) {
        index = sizeof(intervals_ms) / sizeof(intervals_ms[0]) - 1U;
    }
    return intervals_ms[index];
}
