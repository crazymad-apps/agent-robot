#include "terminal_ui.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "agent_robot_board.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"

#define TERMINAL_UI_DRAW_BUFFER_LINES 20U
#define TERMINAL_UI_TICK_PERIOD_MS 2U
#define TERMINAL_UI_TASK_STACK_BYTES 8192U
#define TERMINAL_UI_TASK_PRIORITY 2U
#define TERMINAL_UI_STOPPED_BIT BIT0
#define TERMINAL_UI_READY_BIT BIT1
#define TERMINAL_UI_INITIALIZATION_FAILED_BIT BIT2

struct terminal_ui {
    portMUX_TYPE lock;
    QueueHandle_t projections;
    EventGroupHandle_t lifecycle_events;
    TaskHandle_t task;
    esp_lcd_panel_io_handle_t panel_io;
    esp_lcd_panel_handle_t panel;
    bool spi_bus_initialized;
    bool started;
    bool stop_requested;
};

static bool projection_valid(const terminal_ui_projection_t *projection)
{
    if (projection == NULL || projection->state > TERMINAL_UI_STATE_ERROR ||
        projection->error > TERMINAL_UI_ERROR_AUDIO) {
        return false;
    }
    if (!projection->pairing_code_visible) {
        return true;
    }
    if (projection->pairing_code[DEVICE_PAIRING_CODE_DIGITS] != '\0') {
        return false;
    }
    for (size_t index = 0U; index < DEVICE_PAIRING_CODE_DIGITS; ++index) {
        if (projection->pairing_code[index] < '0' || projection->pairing_code[index] > '9') {
            return false;
        }
    }
    return true;
}

static bool flush_complete(
    esp_lcd_panel_io_handle_t panel_io,
    esp_lcd_panel_io_event_data_t *event_data,
    void *context)
{
    (void)panel_io;
    (void)event_data;
    lv_display_flush_ready(context);
    return false;
}

static void display_flush(
    lv_display_t *display,
    const lv_area_t *area,
    uint8_t *pixels)
{
    const int32_t width = area->x2 - area->x1 + 1;
    const int32_t height = area->y2 - area->y1 + 1;
    lv_draw_sw_rgb565_swap(pixels, (uint32_t)(width * height));
    const esp_err_t result = esp_lcd_panel_draw_bitmap(
        lv_display_get_user_data(display),
        area->x1,
        area->y1,
        area->x2 + 1,
        area->y2 + 1,
        pixels);
    if (result != ESP_OK) {
        lv_display_flush_ready(display);
    }
}

static void tick_increment(void *context)
{
    (void)context;
    lv_tick_inc(TERMINAL_UI_TICK_PERIOD_MS);
}

typedef struct {
    lv_obj_t *state;
    lv_obj_t *pairing_code;
    lv_obj_t *detail;
    lv_obj_t *indicator;
} terminal_ui_page_t;

static terminal_ui_page_t page_create(lv_display_t *display)
{
    terminal_ui_page_t page = {0};
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B1020), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "AGENT ROBOT");
    lv_obj_set_style_text_color(title, lv_color_hex(0xF5F7FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 22);

    page.indicator = lv_obj_create(screen);
    lv_obj_set_size(page.indicator, 12, 12);
    lv_obj_align(page.indicator, LV_ALIGN_TOP_MID, -65, 62);
    lv_obj_set_style_border_width(page.indicator, 0, 0);
    lv_obj_set_style_radius(page.indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(page.indicator, LV_OBJ_FLAG_SCROLLABLE);

    page.state = lv_label_create(screen);
    lv_obj_align(page.state, LV_ALIGN_TOP_MID, 8, 60);

    page.pairing_code = lv_label_create(screen);
    lv_obj_set_style_text_font(page.pairing_code, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(page.pairing_code, lv_color_hex(0xF5F7FF), 0);
    lv_obj_align(page.pairing_code, LV_ALIGN_CENTER, 0, 4);

    page.detail = lv_label_create(screen);
    lv_obj_set_width(page.detail, 210);
    lv_obj_set_style_text_align(page.detail, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(page.detail, lv_color_hex(0x8FA3C7), 0);
    lv_obj_align(page.detail, LV_ALIGN_BOTTOM_MID, 0, -34);
    return page;
}

static const char *error_detail(terminal_ui_error_t error)
{
    switch (error) {
        case TERMINAL_UI_ERROR_MULTIPLE_HOSTS:
            return "MULTIPLE HOSTS";
        case TERMINAL_UI_ERROR_CERTIFICATE:
            return "CERTIFICATE";
        case TERMINAL_UI_ERROR_IDENTITY:
            return "IDENTITY";
        case TERMINAL_UI_ERROR_PROTOCOL:
            return "PROTOCOL";
        case TERMINAL_UI_ERROR_NETWORK:
            return "NETWORK";
        case TERMINAL_UI_ERROR_AUDIO:
            return "AUDIO";
        default:
            return "CHECK CONNECTION";
    }
}

static void projection_apply(
    const terminal_ui_page_t *page,
    const terminal_ui_projection_t *projection)
{
    const char *state = "STARTING";
    const char *detail = "INITIALIZING";
    uint32_t color = 0x8FA3C7;
    switch (projection->state) {
        case TERMINAL_UI_STATE_CONNECTING:
            state = "CONNECTING";
            detail = "LOOKING FOR HOST";
            break;
        case TERMINAL_UI_STATE_WAITING_FOR_PAIRING:
            state = "WAIT PAIR";
            detail = "OPEN PAIRING ON DESKTOP";
            color = 0xFFB84D;
            break;
        case TERMINAL_UI_STATE_PAIRING:
            state = "PAIRING";
            detail = "CONFIRM ON DESKTOP";
            color = 0xFFB84D;
            break;
        case TERMINAL_UI_STATE_READY:
            state = "READY";
            detail = "PRESS TO TALK";
            color = 0x35D07F;
            break;
        case TERMINAL_UI_STATE_LISTENING:
            state = "LISTENING";
            detail = "RELEASE WHEN FINISHED";
            color = 0x35D07F;
            break;
        case TERMINAL_UI_STATE_THINKING:
            state = "THINKING";
            detail = "PLEASE WAIT";
            color = 0x4D8DFF;
            break;
        case TERMINAL_UI_STATE_SPEAKING:
            state = "SPEAKING";
            detail = "PRESS TO INTERRUPT";
            color = 0x4D8DFF;
            break;
        case TERMINAL_UI_STATE_CHECK_DESKTOP:
            state = "CHECK DESKTOP";
            detail = "ACTION REQUIRED";
            color = 0xFFB84D;
            break;
        case TERMINAL_UI_STATE_PAIRING_RESET_ARMED:
            state = "RELEASE TO RESET";
            detail = "PAIRING WILL BE CLEARED";
            color = 0xFFB84D;
            break;
        case TERMINAL_UI_STATE_ERROR:
            state = "ERROR";
            detail = error_detail(projection->error);
            color = 0xFF5D6C;
            break;
        case TERMINAL_UI_STATE_STARTING:
        default:
            break;
    }
    lv_label_set_text(page->state, state);
    lv_obj_set_style_text_color(page->state, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(page->indicator, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(page->indicator, LV_OPA_COVER, 0);
    lv_label_set_text(
        page->pairing_code,
        projection->pairing_code_visible ? projection->pairing_code : "");
    lv_label_set_text(page->detail, detail);
}

static bool stop_requested(terminal_ui_t *ui)
{
    taskENTER_CRITICAL(&ui->lock);
    const bool requested = ui->stop_requested;
    taskEXIT_CRITICAL(&ui->lock);
    return requested;
}

static void terminal_ui_task(void *context)
{
    terminal_ui_t *ui = context;
    void *first_buffer = NULL;
    void *second_buffer = NULL;
    lv_display_t *display = NULL;
    esp_timer_handle_t tick_timer = NULL;
    bool initialized = false;

    lv_init();
    display = lv_display_create(
        AGENT_ROBOT_BOARD_LCD_WIDTH,
        AGENT_ROBOT_BOARD_LCD_HEIGHT);
    const size_t buffer_size = AGENT_ROBOT_BOARD_LCD_WIDTH *
                               TERMINAL_UI_DRAW_BUFFER_LINES *
                               sizeof(lv_color16_t);
    if (display != NULL) {
        first_buffer = spi_bus_dma_memory_alloc(
            AGENT_ROBOT_BOARD_LCD_SPI_HOST,
            buffer_size,
            0U);
        second_buffer = spi_bus_dma_memory_alloc(
            AGENT_ROBOT_BOARD_LCD_SPI_HOST,
            buffer_size,
            0U);
    }
    if (display != NULL && first_buffer != NULL && second_buffer != NULL) {
        lv_display_set_buffers(
            display,
            first_buffer,
            second_buffer,
            buffer_size,
            LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_user_data(display, ui->panel);
        lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
        lv_display_set_flush_cb(display, display_flush);
        const esp_lcd_panel_io_callbacks_t callbacks = {
            .on_color_trans_done = flush_complete,
        };
        const esp_timer_create_args_t tick_config = {
            .callback = tick_increment,
            .name = "terminal_ui_tick",
        };
        if (esp_lcd_panel_io_register_event_callbacks(
                ui->panel_io,
                &callbacks,
                display) == ESP_OK &&
            esp_timer_create(&tick_config, &tick_timer) == ESP_OK &&
            esp_timer_start_periodic(
                tick_timer,
                TERMINAL_UI_TICK_PERIOD_MS * 1000U) == ESP_OK) {
            const terminal_ui_page_t page = page_create(display);
            terminal_ui_projection_t projection;
            initialized = true;
            xEventGroupSetBits(ui->lifecycle_events, TERMINAL_UI_READY_BIT);
            while (!stop_requested(ui)) {
                if (xQueueReceive(ui->projections, &projection, 0U) == pdTRUE) {
                    projection_apply(&page, &projection);
                }
                uint32_t delay_ms = lv_timer_handler();
                if (delay_ms < 10U) {
                    delay_ms = 10U;
                } else if (delay_ms > 100U) {
                    delay_ms = 100U;
                }
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(delay_ms));
            }
        }
    }

    if (!initialized) {
        xEventGroupSetBits(
            ui->lifecycle_events,
            TERMINAL_UI_INITIALIZATION_FAILED_BIT);
    }

    if (tick_timer != NULL) {
        (void)esp_timer_stop(tick_timer);
        (void)esp_timer_delete(tick_timer);
    }
    if (display != NULL) {
        lv_display_delete(display);
    }
    heap_caps_free(first_buffer);
    heap_caps_free(second_buffer);
    taskENTER_CRITICAL(&ui->lock);
    ui->task = NULL;
    ui->started = false;
    taskEXIT_CRITICAL(&ui->lock);
    xEventGroupSetBits(ui->lifecycle_events, TERMINAL_UI_STOPPED_BIT);
    vTaskDelete(NULL);
}

terminal_ui_result_t terminal_ui_create(terminal_ui_t **ui)
{
    if (ui == NULL || *ui != NULL) {
        return TERMINAL_UI_RESULT_INVALID_ARGUMENT;
    }
    terminal_ui_t *created = calloc(1U, sizeof(*created));
    if (created == NULL) {
        return TERMINAL_UI_RESULT_NO_MEMORY;
    }
    created->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    created->projections = xQueueCreate(1U, sizeof(terminal_ui_projection_t));
    created->lifecycle_events = xEventGroupCreate();
    if (created->projections == NULL || created->lifecycle_events == NULL) {
        terminal_ui_destroy(created);
        return TERMINAL_UI_RESULT_NO_MEMORY;
    }

    const spi_bus_config_t bus_config = {
        .sclk_io_num = AGENT_ROBOT_BOARD_LCD_SCLK_GPIO,
        .mosi_io_num = AGENT_ROBOT_BOARD_LCD_MOSI_GPIO,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = AGENT_ROBOT_BOARD_LCD_WIDTH *
                           TERMINAL_UI_DRAW_BUFFER_LINES * sizeof(uint16_t),
    };
    esp_err_t result = spi_bus_initialize(
        AGENT_ROBOT_BOARD_LCD_SPI_HOST,
        &bus_config,
        SPI_DMA_CH_AUTO);
    if (result == ESP_OK) {
        created->spi_bus_initialized = true;
        const esp_lcd_panel_io_spi_config_t io_config = {
            .cs_gpio_num = AGENT_ROBOT_BOARD_LCD_CS_GPIO,
            .dc_gpio_num = AGENT_ROBOT_BOARD_LCD_DC_GPIO,
            .spi_mode = 0,
            .pclk_hz = AGENT_ROBOT_BOARD_LCD_PIXEL_CLOCK_HZ,
            .trans_queue_depth = 1,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
        };
        result = esp_lcd_new_panel_io_spi(
            AGENT_ROBOT_BOARD_LCD_SPI_HOST,
            &io_config,
            &created->panel_io);
    }
    if (result == ESP_OK) {
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = AGENT_ROBOT_BOARD_LCD_RESET_GPIO,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
        };
        result = esp_lcd_new_panel_st7789(
            created->panel_io,
            &panel_config,
            &created->panel);
    }
    if (result == ESP_OK) {
        result = esp_lcd_panel_reset(created->panel);
    }
    if (result == ESP_OK) {
        result = esp_lcd_panel_init(created->panel);
    }
    if (result == ESP_OK) {
        result = esp_lcd_panel_invert_color(created->panel, true);
    }
    if (result == ESP_OK) {
        result = esp_lcd_panel_set_gap(created->panel, 0, 0);
    }
    if (result == ESP_OK) {
        result = esp_lcd_panel_disp_on_off(created->panel, true);
    }
    if (result != ESP_OK) {
        terminal_ui_destroy(created);
        return TERMINAL_UI_RESULT_HARDWARE_FAILURE;
    }
    const terminal_ui_projection_t starting = {
        .state = TERMINAL_UI_STATE_STARTING,
    };
    (void)xQueueOverwrite(created->projections, &starting);
    *ui = created;
    return TERMINAL_UI_RESULT_OK;
}

terminal_ui_result_t terminal_ui_start(terminal_ui_t *ui)
{
    if (ui == NULL) {
        return TERMINAL_UI_RESULT_INVALID_ARGUMENT;
    }
    taskENTER_CRITICAL(&ui->lock);
    if (ui->started) {
        taskEXIT_CRITICAL(&ui->lock);
        return TERMINAL_UI_RESULT_INVALID_STATE;
    }
    ui->stop_requested = false;
    ui->started = true;
    taskEXIT_CRITICAL(&ui->lock);
    xEventGroupClearBits(ui->lifecycle_events, TERMINAL_UI_STOPPED_BIT);
    xEventGroupClearBits(
        ui->lifecycle_events,
        TERMINAL_UI_READY_BIT | TERMINAL_UI_INITIALIZATION_FAILED_BIT);
    if (xTaskCreate(
            terminal_ui_task,
            "terminal_ui",
            TERMINAL_UI_TASK_STACK_BYTES,
            ui,
            TERMINAL_UI_TASK_PRIORITY,
            &ui->task) != pdPASS) {
        taskENTER_CRITICAL(&ui->lock);
        ui->started = false;
        taskEXIT_CRITICAL(&ui->lock);
        return TERMINAL_UI_RESULT_NO_MEMORY;
    }
    const EventBits_t initialization = xEventGroupWaitBits(
        ui->lifecycle_events,
        TERMINAL_UI_READY_BIT | TERMINAL_UI_INITIALIZATION_FAILED_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);
    if ((initialization & TERMINAL_UI_INITIALIZATION_FAILED_BIT) != 0U) {
        return TERMINAL_UI_RESULT_HARDWARE_FAILURE;
    }
    return TERMINAL_UI_RESULT_OK;
}

terminal_ui_result_t terminal_ui_update(
    terminal_ui_t *ui,
    const terminal_ui_projection_t *projection)
{
    if (ui == NULL || !projection_valid(projection)) {
        return TERMINAL_UI_RESULT_INVALID_ARGUMENT;
    }
    if (xQueueOverwrite(ui->projections, projection) != pdPASS) {
        return TERMINAL_UI_RESULT_INVALID_STATE;
    }
    taskENTER_CRITICAL(&ui->lock);
    TaskHandle_t task = ui->task;
    taskEXIT_CRITICAL(&ui->lock);
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    return TERMINAL_UI_RESULT_OK;
}

void terminal_ui_stop(terminal_ui_t *ui)
{
    if (ui == NULL) {
        return;
    }
    taskENTER_CRITICAL(&ui->lock);
    const bool started = ui->started;
    ui->stop_requested = true;
    TaskHandle_t task = ui->task;
    taskEXIT_CRITICAL(&ui->lock);
    if (!started) {
        return;
    }
    if (task != NULL) {
        xTaskNotifyGive(task);
    }
    (void)xEventGroupWaitBits(
        ui->lifecycle_events,
        TERMINAL_UI_STOPPED_BIT,
        pdFALSE,
        pdTRUE,
        portMAX_DELAY);
}

void terminal_ui_destroy(terminal_ui_t *ui)
{
    if (ui == NULL) {
        return;
    }
    terminal_ui_stop(ui);
    if (ui->panel != NULL) {
        (void)esp_lcd_panel_del(ui->panel);
    }
    if (ui->panel_io != NULL) {
        (void)esp_lcd_panel_io_del(ui->panel_io);
    }
    if (ui->spi_bus_initialized) {
        (void)spi_bus_free(AGENT_ROBOT_BOARD_LCD_SPI_HOST);
    }
    if (ui->projections != NULL) {
        vQueueDelete(ui->projections);
    }
    if (ui->lifecycle_events != NULL) {
        vEventGroupDelete(ui->lifecycle_events);
    }
    free(ui);
}
