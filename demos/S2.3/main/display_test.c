#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "lvgl.h"

#include "board_config.h"

#define LCD_HOST SPI2_HOST
#define LCD_WIDTH 240
#define LCD_HEIGHT 240
#define LCD_PIXEL_CLOCK_HZ (10 * 1000 * 1000)
#define LCD_CMD_BITS 8
#define LCD_PARAM_BITS 8
#define SOLID_COLOR_MS 1000
#define ORIENTATION_PATTERN_MS 2500
#define LVGL_DRAW_BUFFER_LINES 20
#define LVGL_TICK_PERIOD_MS 2
#define LVGL_MIN_DELAY_MS 10
#define LVGL_MAX_DELAY_MS 100
#define LVGL_TASK_STACK_SIZE 8192
#define LVGL_TASK_PRIORITY 2

static const char *TAG = "agent_robot_display";

static uint16_t *frame_buffer;

typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io_handle;
} lvgl_task_context_t;

static lvgl_task_context_t lvgl_task_context;

static uint16_t rgb565_wire(uint8_t red, uint8_t green, uint8_t blue)
{
    const uint16_t color =
        ((uint16_t)(red & 0xF8) << 8) |
        ((uint16_t)(green & 0xFC) << 3) |
        ((uint16_t)blue >> 3);
    return (uint16_t)((color << 8) | (color >> 8));
}

static void print_board_info(void)
{
    esp_chip_info_t chip_info;
    uint32_t flash_size = 0;

    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "ESP-IDF version: %s", esp_get_idf_version());
    ESP_LOGI(TAG,
             "Chip: ESP32-S3, cores: %d, silicon revision: %d.%d",
             chip_info.cores,
             chip_info.revision / 100,
             chip_info.revision % 100);
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "Detected flash: %" PRIu32 " MB", flash_size / (1024 * 1024));
    }
    ESP_LOGI(TAG,
             "PSRAM total/free: %u/%u bytes",
             (unsigned int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static esp_err_t create_onboard_rgb(led_strip_handle_t *strip)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = BOARD_ONBOARD_RGB_GPIO,
        .max_leds = BOARD_ONBOARD_RGB_LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags.with_dma = false,
    };
    return led_strip_new_rmt_device(&strip_config, &rmt_config, strip);
}

static void set_status_rgb(led_strip_handle_t strip, uint8_t red, uint8_t green, uint8_t blue)
{
    ESP_ERROR_CHECK(led_strip_set_pixel(strip, 0, red, green, blue));
    ESP_ERROR_CHECK(led_strip_refresh(strip));
}

static esp_lcd_panel_handle_t init_display(esp_lcd_panel_io_handle_t *io_handle_out)
{
    ESP_LOGI(TAG,
             "Initializing SPI display: SCLK=%d MOSI=%d CS=%d DC=%d RST=%d",
             BOARD_LCD_SCLK_GPIO,
             BOARD_LCD_MOSI_GPIO,
             BOARD_LCD_CS_GPIO,
             BOARD_LCD_DC_GPIO,
             BOARD_LCD_RESET_GPIO);

    const spi_bus_config_t bus_config = {
        .sclk_io_num = BOARD_LCD_SCLK_GPIO,
        .mosi_io_num = BOARD_LCD_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = BOARD_LCD_CS_GPIO,
        .dc_gpio_num = BOARD_LCD_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 1,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_PARAM_BITS,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BOARD_LCD_RESET_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel_handle, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    *io_handle_out = io_handle;

    return panel_handle;
}

static void show_solid(esp_lcd_panel_handle_t panel, uint16_t color, const char *name)
{
    for (size_t pixel = 0; pixel < LCD_WIDTH * LCD_HEIGHT; ++pixel) {
        frame_buffer[pixel] = color;
    }
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
        panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, frame_buffer));
    ESP_LOGI(TAG, "Pattern: solid %s", name);
    vTaskDelay(pdMS_TO_TICKS(SOLID_COLOR_MS));
}

static void show_orientation_pattern(esp_lcd_panel_handle_t panel)
{
    const uint16_t black = rgb565_wire(0, 0, 0);
    const uint16_t white = rgb565_wire(255, 255, 255);
    const uint16_t red = rgb565_wire(255, 0, 0);
    const uint16_t green = rgb565_wire(0, 255, 0);
    const uint16_t blue = rgb565_wire(0, 0, 255);
    const uint16_t yellow = rgb565_wire(255, 255, 0);

    for (int y = 0; y < LCD_HEIGHT; ++y) {
        for (int x = 0; x < LCD_WIDTH; ++x) {
            uint16_t color = white;
            if (x < 70 && y < 70) {
                color = red;
            } else if (x >= LCD_WIDTH - 55 && y < 55) {
                color = green;
            } else if (x < 40 && y >= LCD_HEIGHT - 40) {
                color = blue;
            } else if (x >= LCD_WIDTH - 25 && y >= LCD_HEIGHT - 25) {
                color = yellow;
            }
            if ((x >= 116 && x <= 123) || (y >= 116 && y <= 123)) {
                color = black;
            }
            frame_buffer[y * LCD_WIDTH + x] = color;
        }
    }

    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
        panel, 0, 0, LCD_WIDTH, LCD_HEIGHT, frame_buffer));
    ESP_LOGI(TAG,
             "Pattern: orientation corners TL=red TR=green BL=blue BR=yellow");
    vTaskDelay(pdMS_TO_TICKS(ORIENTATION_PATTERN_MS));
}

static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io,
                                    esp_lcd_panel_io_event_data_t *event_data,
                                    void *user_context)
{
    (void)panel_io;
    (void)event_data;
    lv_display_flush_ready((lv_display_t *)user_context);
    return false;
}

static void lvgl_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixel_map)
{
    esp_lcd_panel_handle_t panel = lv_display_get_user_data(display);
    const int width = area->x2 - area->x1 + 1;
    const int height = area->y2 - area->y1 + 1;

    lv_draw_sw_rgb565_swap(pixel_map, width * height);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
        panel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, pixel_map));
}

static void increase_lvgl_tick(void *argument)
{
    (void)argument;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

static void create_status_page(lv_display_t *display)
{
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "AGENT ROBOT");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF5F7FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    lv_obj_t *subtitle = lv_label_create(screen);
    lv_label_set_text(subtitle, "Hardware display check");
    lv_obj_set_style_text_color(subtitle, lv_color_hex(0x8FA3C7), 0);
    lv_obj_align(subtitle, LV_ALIGN_TOP_MID, 0, 42);

    lv_obj_t *card = lv_obj_create(screen);
    lv_obj_set_size(card, 204, 112);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 12);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x151F38), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2A3A5E), 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *status_dot = lv_obj_create(card);
    lv_obj_set_size(status_dot, 14, 14);
    lv_obj_align(status_dot, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_set_style_bg_color(status_dot, lv_color_hex(0x35D07F), 0);
    lv_obj_set_style_bg_opa(status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_dot, 0, 0);
    lv_obj_set_style_radius(status_dot, LV_RADIUS_CIRCLE, 0);

    lv_obj_t *ready = lv_label_create(card);
    lv_label_set_text(ready, "DISPLAY READY");
    lv_obj_set_style_text_color(ready, lv_color_hex(0x35D07F), 0);
    lv_obj_align(ready, LV_ALIGN_TOP_LEFT, 24, -1);

    lv_obj_t *panel_info = lv_label_create(card);
    lv_label_set_text(panel_info, "ST7789   240 x 240");
    lv_obj_set_style_text_color(panel_info, lv_color_hex(0xE8ECF7), 0);
    lv_obj_align(panel_info, LV_ALIGN_TOP_LEFT, 2, 36);

    lv_obj_t *bus_info = lv_label_create(card);
    lv_label_set_text(bus_info, "SPI 10 MHz   RGB565");
    lv_obj_set_style_text_color(bus_info, lv_color_hex(0xAEBBD3), 0);
    lv_obj_align(bus_info, LV_ALIGN_TOP_LEFT, 2, 66);

    lv_obj_t *footer = lv_label_create(screen);
    lv_label_set_text(footer, "S1.2  /  LVGL");
    lv_obj_set_style_text_color(footer, lv_color_hex(0x65789D), 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -16);
}

static lv_display_t *init_lvgl(esp_lcd_panel_handle_t panel,
                               esp_lcd_panel_io_handle_t io_handle)
{
    lv_init();

    lv_display_t *display = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
    ESP_ERROR_CHECK(display != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    const size_t draw_buffer_size =
        LCD_WIDTH * LVGL_DRAW_BUFFER_LINES * sizeof(lv_color16_t);
    void *draw_buffer_1 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_size, 0);
    void *draw_buffer_2 = spi_bus_dma_memory_alloc(LCD_HOST, draw_buffer_size, 0);
    ESP_ERROR_CHECK(
        draw_buffer_1 != NULL && draw_buffer_2 != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    lv_display_set_buffers(display,
                           draw_buffer_1,
                           draw_buffer_2,
                           draw_buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_user_data(display, panel);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, lvgl_flush);

    const esp_lcd_panel_io_callbacks_t callbacks = {
        .on_color_trans_done = notify_lvgl_flush_ready,
    };
    ESP_ERROR_CHECK(
        esp_lcd_panel_io_register_event_callbacks(io_handle, &callbacks, display));

    const esp_timer_create_args_t tick_timer_config = {
        .callback = increase_lvgl_tick,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_timer_config, &tick_timer));
    ESP_ERROR_CHECK(
        esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    create_status_page(display);
    return display;
}

static void lvgl_task(void *argument)
{
    const lvgl_task_context_t *context = (const lvgl_task_context_t *)argument;
    ESP_LOGI(TAG, "LVGL task started with %d-byte stack", LVGL_TASK_STACK_SIZE);
    lv_display_t *display = init_lvgl(context->panel, context->io_handle);
    ESP_LOGI(TAG, "LVGL status page ready");

    while (true) {
        (void)display;
        uint32_t delay_ms = lv_timer_handler();
        if (delay_ms < LVGL_MIN_DELAY_MS) {
            delay_ms = LVGL_MIN_DELAY_MS;
        } else if (delay_ms > LVGL_MAX_DELAY_MS) {
            delay_ms = LVGL_MAX_DELAY_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void run_display_test(void)
{
    led_strip_handle_t status_rgb = NULL;

    ESP_LOGI(TAG, "Agent Robot single-device display test started");
    ESP_LOGI(TAG, "Demo version: %s; board: %s", DEMO_VERSION, BOARD_NAME);
    print_board_info();

    ESP_ERROR_CHECK(gpio_reset_pin(BOARD_AMP_ENABLE_GPIO));
    ESP_ERROR_CHECK(gpio_set_direction(BOARD_AMP_ENABLE_GPIO, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(BOARD_AMP_ENABLE_GPIO, 0));
    ESP_LOGI(TAG, "Amplifier held disabled on GPIO%d", BOARD_AMP_ENABLE_GPIO);

    ESP_ERROR_CHECK(create_onboard_rgb(&status_rgb));
    set_status_rgb(status_rgb, 24, 0, 0);

    frame_buffer = heap_caps_malloc(
        LCD_WIDTH * LCD_HEIGHT * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_ERROR_CHECK(frame_buffer != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_handle_t panel = init_display(&io_handle);
    set_status_rgb(status_rgb, 0, 24, 0);
    ESP_LOGI(TAG, "ST7789 ready: 240x240 RGB565 at 10 MHz");

    ESP_LOGI(TAG, "Running one visual pattern cycle before LVGL");
    show_solid(panel, rgb565_wire(255, 0, 0), "red");
    show_solid(panel, rgb565_wire(0, 255, 0), "green");
    show_solid(panel, rgb565_wire(0, 0, 255), "blue");
    show_solid(panel, rgb565_wire(255, 255, 255), "white");
    show_solid(panel, rgb565_wire(0, 0, 0), "black");
    show_orientation_pattern(panel);

    lvgl_task_context.panel = panel;
    lvgl_task_context.io_handle = io_handle;
    ESP_ERROR_CHECK(xTaskCreate(lvgl_task,
                                "lvgl",
                                LVGL_TASK_STACK_SIZE,
                                &lvgl_task_context,
                                LVGL_TASK_PRIORITY,
                                NULL) == pdPASS
                        ? ESP_OK
                        : ESP_ERR_NO_MEM);
}
