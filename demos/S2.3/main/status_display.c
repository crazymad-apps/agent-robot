#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "board_config.h"
#include "status_display.h"

#define LCD_HOST SPI2_HOST
#define LCD_WIDTH 240
#define LCD_HEIGHT 240
#define LCD_PIXEL_CLOCK_HZ (10 * 1000 * 1000)
#define LVGL_DRAW_BUFFER_LINES 20
#define LVGL_TICK_PERIOD_MS 2
#define LVGL_TASK_STACK_SIZE 8192
#define LVGL_TASK_PRIORITY 2

static const char *TAG = "status_display";
static volatile robot_status_t requested_status = ROBOT_STATUS_IDLE;
static volatile robot_network_status_t requested_network_status = ROBOT_NETWORK_SETUP;

typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
} display_context_t;

static display_context_t display_context;

static bool notify_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                               esp_lcd_panel_io_event_data_t *event_data,
                               void *user_context)
{
    (void)panel_io;
    (void)event_data;
    lv_display_flush_ready((lv_display_t *)user_context);
    return false;
}

static void flush_display(lv_display_t *display,
                          const lv_area_t *area,
                          uint8_t *pixels)
{
    const int width = area->x2 - area->x1 + 1;
    const int height = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(pixels, width * height);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
        lv_display_get_user_data(display),
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        pixels));
}

static void increase_tick(void *argument)
{
    (void)argument;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void apply_status(lv_obj_t *label, lv_obj_t *hint, lv_obj_t *dot,
                         robot_status_t status)
{
    const char *status_text = "IDLE";
    const char *hint_text = "Press button to record";
    uint32_t color = 0x4D8DFF;

    switch (status) {
    case ROBOT_STATUS_IDLE:
        break;
    case ROBOT_STATUS_LISTENING:
        status_text = "LISTENING";
        hint_text = "Speak for 5 seconds";
        color = 0x35D07F;
        break;
    case ROBOT_STATUS_PLAYING:
        status_text = "PLAYING";
        hint_text = "Local microphone playback";
        color = 0xFFB84D;
        break;
    case ROBOT_STATUS_ERROR:
        status_text = "ERROR";
        hint_text = "Check serial log";
        color = 0xFF5D6C;
        break;
    }

    lv_label_set_text(label, status_text);
    lv_label_set_text(hint, hint_text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
}

static void apply_network_status(lv_obj_t *label, robot_network_status_t status)
{
    const char *text = "WIFI SETUP";
    uint32_t color = 0xFFB84D;
    switch (status) {
    case ROBOT_NETWORK_SETUP:
        break;
    case ROBOT_NETWORK_CONNECTING:
        text = "WIFI CONNECTING";
        color = 0x8FA3C7;
        break;
    case ROBOT_NETWORK_CONNECTED:
        text = "WIFI CONNECTED";
        color = 0x35D07F;
        break;
    case ROBOT_NETWORK_DISCONNECTED:
        text = "WIFI RETRY";
        color = 0xFFB84D;
        break;
    case ROBOT_NETWORK_ERROR:
        text = "WIFI ERROR";
        color = 0xFF5D6C;
        break;
    }
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

static void display_task(void *argument)
{
    const display_context_t *context = (const display_context_t *)argument;
    lv_init();

    lv_display_t *display = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    if (display == NULL) {
        ESP_LOGE(TAG, "lv_display_create failed");
        vTaskDelete(NULL);
        return;
    }

    const size_t buffer_size =
        LCD_WIDTH * LVGL_DRAW_BUFFER_LINES * sizeof(lv_color16_t);
    void *buffer_1 = spi_bus_dma_memory_alloc(LCD_HOST, buffer_size, 0);
    void *buffer_2 = spi_bus_dma_memory_alloc(LCD_HOST, buffer_size, 0);
    if (buffer_1 == NULL || buffer_2 == NULL) {
        ESP_LOGE(TAG, "LVGL DMA buffer allocation failed");
        vTaskDelete(NULL);
        return;
    }

    lv_display_set_buffers(display,
                           buffer_1,
                           buffer_2,
                           buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display, context->panel);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, flush_display);

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = notify_flush_ready,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(
        context->io, &callbacks, display));

    const esp_timer_create_args_t tick_config = {
        .callback = increase_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_config, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(
        tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "AGENT ROBOT");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF5F7FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, 204, 118);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 8);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x151F38), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A3A5E), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dot = lv_obj_create(card);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 2, 2);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *status_label = lv_label_create(card);
    lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 25, 0);

    lv_obj_t *hint_label = lv_label_create(card);
    lv_obj_set_style_text_color(hint_label, lv_color_hex(0xD7DEED), 0);
    lv_obj_align(hint_label, LV_ALIGN_TOP_LEFT, 2, 40);

    lv_obj_t *format_label = lv_label_create(card);
    lv_label_set_text(format_label, "16 kHz  /  mono  /  PCM16");
    lv_obj_set_style_text_color(format_label, lv_color_hex(0x8FA3C7), 0);
    lv_obj_align(format_label, LV_ALIGN_TOP_LEFT, 2, 72);

    lv_obj_t *footer = lv_label_create(screen);
    lv_label_set_text(footer, "S2.1  WIFI + LOOPBACK");
    lv_obj_set_style_text_color(footer, lv_color_hex(0x65789D), 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -18);

    lv_obj_t *network_label = lv_label_create(screen);
    lv_obj_align(network_label, LV_ALIGN_BOTTOM_MID, 0, -42);

    robot_status_t applied_status = (robot_status_t)-1;
    robot_network_status_t applied_network_status = (robot_network_status_t)-1;
    ESP_LOGI(TAG, "LVGL state page ready");
    while (true) {
        const robot_status_t next_status = requested_status;
        if (next_status != applied_status) {
            apply_status(status_label, hint_label, dot, next_status);
            applied_status = next_status;
        }
        const robot_network_status_t next_network_status = requested_network_status;
        if (next_network_status != applied_network_status) {
            apply_network_status(network_label, next_network_status);
            applied_network_status = next_network_status;
        }

        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms < 10) {
            delay_ms = 10;
        } else if (delay_ms > 100) {
            delay_ms = 100;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

esp_err_t status_display_init(void)
{
    const spi_bus_config_t bus_config = {
        .sclk_io_num = BOARD_LCD_SCLK_GPIO,
        .mosi_io_num = BOARD_LCD_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
    };
    esp_err_t result = spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (result != ESP_OK) {
        return result;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BOARD_LCD_CS_GPIO,
        .dc_gpio_num = BOARD_LCD_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    result = esp_lcd_new_panel_io_spi(
        LCD_HOST, &io_config, &display_context.io);
    if (result != ESP_OK) {
        return result;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    result = esp_lcd_new_panel_st7789(
        display_context.io, &panel_config, &display_context.panel);
    if (result != ESP_OK) {
        return result;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(display_context.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(display_context.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(display_context.panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(display_context.panel, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(display_context.panel, true));

    return xTaskCreate(display_task,
                       "status_display",
                       LVGL_TASK_STACK_SIZE,
                       &display_context,
                       LVGL_TASK_PRIORITY,
                       NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

void status_display_set(robot_status_t status)
{
    requested_status = status;
}

void status_display_set_network(robot_network_status_t status)
{
    requested_network_status = status;
}
