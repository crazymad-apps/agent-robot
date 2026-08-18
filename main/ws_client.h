#ifndef AGENT_ROBOT_WS_CLIENT_H
#define AGENT_ROBOT_WS_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * 初始化 WebSocket 客户端（异步连接，自动重连）。
 * 连接地址取自 menuconfig: AGENT_WS_URI。
 */
void ws_client_init(void);

/** 当前是否已连接 */
bool ws_is_connected(void);

/** 发送文本帧（JSON 信令）。返回实际发送字节数，失败返回 -1 */
int ws_send_text(const char *text);

/** 发送二进制帧（音频数据）。返回实际发送字节数，失败返回 -1 */
int ws_send_bin(const uint8_t *data, size_t len);

#endif /* AGENT_ROBOT_WS_CLIENT_H */
