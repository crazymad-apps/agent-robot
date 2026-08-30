#pragma once

#include <stddef.h>
#include <stdint.h>

#include "device_capabilities.h"
#include "device_protocol_result.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_MEDIA_CLIENT_INPUT_ID_MAX_BYTES 256U
#define DEVICE_MEDIA_OUTPUT_ID_MAX_BYTES 256U

device_protocol_result_t device_media_control_encode_listen_start(
    const char *message_id, const char *client_input_id, uint32_t stream_id,
    device_output_preference_t output_preference, char *encoded, size_t encoded_capacity,
    size_t *encoded_size);

device_protocol_result_t
device_media_control_encode_listen_stop(const char *message_id, uint32_t stream_id,
                                        uint32_t last_sequence, char *encoded,
                                        size_t encoded_capacity, size_t *encoded_size);

device_protocol_result_t
device_media_control_encode_listen_cancel(const char *message_id, uint32_t stream_id, char *encoded,
                                          size_t encoded_capacity, size_t *encoded_size);

device_protocol_result_t
device_media_control_encode_playback_cancel(const char *message_id, const char *output_id,
                                            uint32_t stream_id, char *encoded,
                                            size_t encoded_capacity, size_t *encoded_size);

#ifdef __cplusplus
}
#endif
