#ifndef AGENT_ROBOT_UI_H
#define AGENT_ROBOT_UI_H

#include <stdbool.h>

typedef enum {
    UI_STATE_DISCONNECTED = 0, /* 未连接远端 Agent */
    UI_STATE_CONNECTED,        /* 已连接，空闲 */
    UI_STATE_LISTENING,        /* 监听中（用户说话） */
    UI_STATE_THINKING,         /* Agent 处理中 */
    UI_STATE_SPEAKING,         /* Agent 回复播放中 */
    UI_STATE_COUNT
} ui_state_t;

/** 初始化显示。M0 阶段为桩实现（串口日志） */
void ui_init(void);

/** 设置终端状态（驱动 UI 界面/动画） */
void ui_set_state(ui_state_t state);

ui_state_t ui_get_state(void);

/** 状态枚举 → 字符串（协议/日志用） */
const char *ui_state_name(ui_state_t state);

/** 字符串 → 状态枚举，未知返回 UI_STATE_CONNECTED */
ui_state_t ui_state_from_string(const char *name);

/** 显示一段对话文本 */
void ui_show_text(const char *text);

#endif /* AGENT_ROBOT_UI_H */
