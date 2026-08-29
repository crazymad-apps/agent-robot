#include "device_connection_supervisor.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    device_connection_supervisor_t supervisor;
    device_connection_supervisor_initialize(&supervisor);
    assert(supervisor.state == DEVICE_CONNECTION_STATE_WIFI_DOWN);

    uint64_t epoch = 0U;
    device_connection_supervisor_got_ip(&supervisor, 0U);
    assert(supervisor.state == DEVICE_CONNECTION_STATE_DISCOVERING);
    assert(device_connection_supervisor_connect_discovered(&supervisor, &epoch));
    assert(epoch != 0U);
    assert(!device_connection_supervisor_connect_discovered(&supervisor, &epoch));
    assert(device_connection_supervisor_transport_ready(&supervisor, epoch));
    assert(device_connection_supervisor_authenticated(&supervisor, epoch, 100U));

    assert(device_connection_supervisor_receive_activity(&supervisor, epoch, 15100U));
    assert(device_connection_supervisor_poll(&supervisor, 45099U, 200U, &epoch) ==
           DEVICE_CONNECTION_ACTION_NONE);
    assert(device_connection_supervisor_poll(&supervisor, 45100U, 200U, &epoch) ==
           DEVICE_CONNECTION_ACTION_NONE);
    assert(supervisor.state == DEVICE_CONNECTION_STATE_BACKOFF);
    const uint64_t first_backoff = supervisor.backoff_until_ms;
    assert(first_backoff == 46100U);

    assert(!device_connection_supervisor_fail(
        &supervisor, epoch, DEVICE_CONNECTION_FAILURE_TRANSIENT, 45101U, 200U));
    assert(supervisor.backoff_until_ms == first_backoff);
    assert(device_connection_supervisor_poll(&supervisor, first_backoff, 0U, &epoch) ==
           DEVICE_CONNECTION_ACTION_CONNECT_CACHED);
    const uint64_t retry_epoch = epoch;
    assert(device_connection_supervisor_fail(
        &supervisor, retry_epoch, DEVICE_CONNECTION_FAILURE_TRANSIENT, 47000U, 0U));
    assert(device_connection_supervisor_poll(
               &supervisor, supervisor.backoff_until_ms, 0U, &epoch) ==
           DEVICE_CONNECTION_ACTION_DISCOVER);

    assert(device_connection_supervisor_connect_discovered(&supervisor, &epoch));
    assert(device_connection_supervisor_fail(
        &supervisor, epoch, DEVICE_CONNECTION_FAILURE_CERTIFICATE, 50000U, 0U));
    assert(supervisor.state == DEVICE_CONNECTION_STATE_BLOCKED);
    assert(supervisor.block_reason == DEVICE_CONNECTION_BLOCK_CERTIFICATE);
    assert(device_connection_supervisor_poll(&supervisor, 100000U, 0U, &epoch) ==
           DEVICE_CONNECTION_ACTION_NONE);

    device_connection_supervisor_wifi_down(&supervisor, 100001U);
    assert(supervisor.state == DEVICE_CONNECTION_STATE_WIFI_DOWN);
    assert(!device_connection_supervisor_transport_ready(&supervisor, epoch));
    device_connection_supervisor_got_ip(&supervisor, 100002U);

    for (size_t duplicate = 0U; duplicate < 3U; ++duplicate) {
        assert(supervisor.state == DEVICE_CONNECTION_STATE_DISCOVERING);
        assert(device_connection_supervisor_connect_discovered(&supervisor, &epoch));
        assert(device_connection_supervisor_fail(
            &supervisor,
            epoch,
            DEVICE_CONNECTION_FAILURE_DUPLICATE,
            110000U + duplicate * 30000U,
            0U));
        if (duplicate < 2U) {
            assert(device_connection_supervisor_poll(
                       &supervisor, supervisor.backoff_until_ms, 0U, &epoch) ==
                   DEVICE_CONNECTION_ACTION_DISCOVER);
        }
    }
    assert(supervisor.state == DEVICE_CONNECTION_STATE_BLOCKED);
    assert(supervisor.block_reason == DEVICE_CONNECTION_BLOCK_DUPLICATE);

    puts("device connection supervisor tests passed");
    return 0;
}
