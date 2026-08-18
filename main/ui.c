/*
 * 显示/状态层（桩实现）。
 *
 * TODO(M3): 接入真实显示。
 *   - 方案 A: LVGL 驱动小屏（BOX-3 自带触摸屏）
 *   - 方案 B: ESP32 内置 Web Server + 前端页面（手机/浏览器查看）
 * 目前以串口日志呈现状态变化与对话文本。
 */
#include <string.h>

#include "esp_log.h"

#include "ui.h"

static const char *TAG = "ui";

static ui_state_t s_state = UI_STATE_DISCONNECTED;

static const char *const s_state_names[UI_STATE_COUNT] = {
    [UI_STATE_DISCONNECTED] = "disconnected",
    [UI_STATE_CONNECTED]    = "connected",
    [UI_STATE_LISTENING]    = "listening",
    [UI_STATE_THINKING]     = "thinking",
    [UI_STATE_SPEAKING]     = "speaking",
};

void ui_init(void)
{
    ESP_LOGI(TAG, "ui stub initialized");
}

void ui_set_state(ui_state_t state)
{
    if (state >= UI_STATE_COUNT) {
        return;
    }
    if (state == s_state) {
        return;
    }
    s_state = state;
    ESP_LOGI(TAG, "state -> %s", s_state_names[s_state]);
    /* TODO(M3): 刷新屏幕/动画 */
}

ui_state_t ui_get_state(void)
{
    return s_state;
}

const char *ui_state_name(ui_state_t state)
{
    if (state >= UI_STATE_COUNT) {
        return "unknown";
    }
    return s_state_names[state];
}

ui_state_t ui_state_from_string(const char *name)
{
    for (int i = 0; i < UI_STATE_COUNT; i++) {
        if (strcmp(name, s_state_names[i]) == 0) {
            return (ui_state_t)i;
        }
    }
    return UI_STATE_CONNECTED;
}

void ui_show_text(const char *text)
{
    ESP_LOGI(TAG, "text: %s", text);
    /* TODO(M3): 上屏显示 */
}
