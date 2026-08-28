#pragma once

/* Hardware facts for the board verified in docs/resources. */
#define BOARD_NAME "ESP32-S3-DevKitC-1 v1.1 N8R8"
#define DEMO_VERSION "S2.3-coexist.1"

#define BOARD_ONBOARD_RGB_GPIO 38
#define BOARD_ONBOARD_RGB_LED_COUNT 1

/* S1 flying-wire I2S output baseline. */
#define BOARD_I2S_BCLK_GPIO 4
#define BOARD_I2S_WS_GPIO 5
#define BOARD_I2S_TX_GPIO 15
#define BOARD_I2S_RX_GPIO 6
#define BOARD_AMP_ENABLE_GPIO 16

/* S1.4 active-low PTT button. The switch connects GPIO21 to GND. */
#define BOARD_PTT_BUTTON_GPIO 21

/* S1 flying-wire SPI display baseline. BLK is fixed to 3.3V for first light. */
#define BOARD_LCD_SCLK_GPIO 12
#define BOARD_LCD_MOSI_GPIO 11
#define BOARD_LCD_CS_GPIO 10
#define BOARD_LCD_DC_GPIO 9
#define BOARD_LCD_RESET_GPIO 8
#define BOARD_LCD_BACKLIGHT_GPIO 7
