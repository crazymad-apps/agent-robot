#ifndef AGENT_ROBOT_PROTOCOL_H
#define AGENT_ROBOT_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#include "esp_websocket_client.h"

/** 终端上线握手：发送 hello 信令 */
void protocol_send_hello(esp_websocket_client_handle_t client);

/** 处理一帧来自远端的消息（文本=JSON 信令，二进制=音频） */
void protocol_handle_frame(esp_websocket_client_handle_t client,
                           const esp_websocket_event_data_t *data);

#endif /* AGENT_ROBOT_PROTOCOL_H */
