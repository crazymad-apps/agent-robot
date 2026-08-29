#include "network_reconnect_policy.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    static const uint32_t expected_ms[] = {
        1000U,
        1000U,
        2000U,
        4000U,
        8000U,
        16000U,
        30000U,
        30000U,
    };
    for (uint32_t attempt = 0U; attempt < sizeof(expected_ms) / sizeof(expected_ms[0]); ++attempt) {
        assert(network_reconnect_policy_delay_ms(attempt) == expected_ms[attempt]);
    }
    assert(network_reconnect_policy_delay_ms(UINT32_MAX) == 30000U);
    puts("network reconnect policy tests passed");
    return 0;
}
