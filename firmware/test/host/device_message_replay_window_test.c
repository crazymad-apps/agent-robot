#include "device_message_replay_window.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    device_message_replay_window_t window;
    device_message_replay_window_initialize(&window);

    assert(device_message_replay_window_accept(&window, "message-0")
           == DEVICE_PROTOCOL_RESULT_OK);
    assert(device_message_replay_window_accept(&window, "message-0")
           == DEVICE_PROTOCOL_RESULT_DUPLICATE_MESSAGE_ID);
    assert(device_message_replay_window_accept(&window, " ")
           == DEVICE_PROTOCOL_RESULT_INVALID_ARGUMENT);

    device_message_replay_window_initialize(&window);
    char message_id[32] = {0};
    for (size_t index = 0U; index < DEVICE_MESSAGE_REPLAY_WINDOW_CAPACITY; ++index) {
        const int written = snprintf(message_id, sizeof(message_id), "message-%zu", index);
        assert(written > 0 && (size_t)written < sizeof(message_id));
        assert(device_message_replay_window_accept(&window, message_id)
               == DEVICE_PROTOCOL_RESULT_OK);
    }
    assert(window.message_count == DEVICE_MESSAGE_REPLAY_WINDOW_CAPACITY);
    assert(device_message_replay_window_accept(&window, "message-0")
           == DEVICE_PROTOCOL_RESULT_DUPLICATE_MESSAGE_ID);
    assert(device_message_replay_window_accept(&window, "message-256")
           == DEVICE_PROTOCOL_RESULT_OK);
    assert(device_message_replay_window_accept(&window, "message-0")
           == DEVICE_PROTOCOL_RESULT_OK);
    assert(device_message_replay_window_accept(&window, "message-1")
           == DEVICE_PROTOCOL_RESULT_OK);
    assert(device_message_replay_window_accept(&window, "message-256")
           == DEVICE_PROTOCOL_RESULT_DUPLICATE_MESSAGE_ID);

    char maximum_message_id[DEVICE_MESSAGE_ID_MAX_BYTES + 1U];
    memset(maximum_message_id, 'x', sizeof(maximum_message_id) - 1U);
    maximum_message_id[sizeof(maximum_message_id) - 1U] = '\0';
    device_message_replay_window_initialize(&window);
    assert(device_message_replay_window_accept(&window, maximum_message_id)
           == DEVICE_PROTOCOL_RESULT_OK);

    puts("device message replay window boundary checks passed");
    return 0;
}
