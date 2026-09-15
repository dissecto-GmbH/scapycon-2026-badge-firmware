#pragma once

/* Pin map derived from netlist.net — update when schematic changes. */

#define BADGE_LCD_PIN_RST   0
#define BADGE_LCD_PIN_MOSI  1
#define BADGE_LCD_PIN_BL    3
#define BADGE_LCD_PIN_SCLK  6
#define BADGE_LCD_PIN_DC    8
#define BADGE_LCD_PIN_CS    9

#define BADGE_LED_PIN       10
#define BADGE_LED_COUNT     6

#define BADGE_USB_PIN_DM    13
#define BADGE_USB_PIN_DP    14

#define BADGE_VBUS_SNS_PIN  2

#define BADGE_FLIPPER_PIN_SDA   4
#define BADGE_FLIPPER_PIN_SCL   5
#define BADGE_FLIPPER_PIN_MISO  7
#define BADGE_FLIPPER_PIN_CS    25
#define BADGE_FLIPPER_PIN_MOSI  26
#define BADGE_FLIPPER_PIN_SCK   27
#define BADGE_FLIPPER_PIN_PA13  23
#define BADGE_FLIPPER_PIN_PA14  24

/* Active-low tact switches to GND; SW_RESET = EN (hardware reset). */
#define BADGE_BTN_PIN_BOOT  28
#define BADGE_BTN_PIN_A     BADGE_FLIPPER_PIN_SCK
#define BADGE_BTN_PIN_B     BADGE_FLIPPER_PIN_MOSI
#define BADGE_BTN_PIN_C     BADGE_FLIPPER_PIN_PA14
#define BADGE_BTN_PIN_D     BADGE_FLIPPER_PIN_PA13

/* LCD_BL (GPIO3) via LEDC. Dim on battery to save power. */
#define BADGE_BACKLIGHT_USB_PERCENT     100
#define BADGE_BACKLIGHT_BATTERY_PERCENT 50
/* NeoPixel brightness ceiling — matches backlight battery saver. */
#define BADGE_LED_USB_PERCENT           100
#define BADGE_LED_BATTERY_PERCENT       50

/* VBUS_SNS (GPIO2) ADC1_CH1 — above this ⇒ USB/VBUS present. */
#define BADGE_VBUS_ADC_PRESENT_MV       800

/* HS20HS072RX: ST7789, 240×320 panel mounted landscape → 320×240 */
#define BADGE_LCD_H_RES     320
#define BADGE_LCD_V_RES     240
