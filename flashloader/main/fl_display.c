#include "fl_display.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "fl_font.h"

static const char *TAG = "fl_disp";

#define LCD_HOST           SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define FILL_STRIP_LINES   20
#define LOG_MAX_LINES      18
#define LOG_SCALE          1
#define LOG_FG             0xFFFF /* white */
#define LOG_BG             0x0000 /* black */
#define BL_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER      LEDC_TIMER_0
#define BL_LEDC_CHANNEL    LEDC_CHANNEL_0
#define BL_LEDC_RES        LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ_HZ    5000

static uint16_t *s_fill_buf;
static bool s_ready;
static bool s_ledc_ready;
static int s_log_line;
static esp_lcd_panel_handle_t s_panel;

static uint16_t rgb565_be(uint16_t color)
{
    return (uint16_t)((color >> 8) | (color << 8));
}

void fl_display_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    if (s_ledc_ready) {
        uint32_t max_duty = (1u << BL_LEDC_RES) - 1u;
        uint32_t duty = (max_duty * percent) / 100u;
        ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
        ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    } else {
        gpio_set_level(BADGE_LCD_PIN_BL, percent > 0 ? 1 : 0);
    }
}

bool fl_display_ready(void)
{
    return s_ready;
}

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer) != ESP_OK) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << BADGE_LCD_PIN_BL,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&io);
        return ESP_OK;
    }
    ledc_channel_config_t ch = {
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .gpio_num = BADGE_LCD_PIN_BL,
        .duty = 0,
    };
    if (ledc_channel_config(&ch) != ESP_OK) {
        return ESP_OK;
    }
    s_ledc_ready = true;
    return ESP_OK;
}

esp_err_t fl_display_init(esp_lcd_panel_handle_t *out_panel)
{
    backlight_init();
    fl_display_set_brightness(BADGE_BACKLIGHT_USB_PERCENT);

    spi_bus_config_t bus = {
        .sclk_io_num = BADGE_LCD_PIN_SCLK,
        .mosi_io_num = BADGE_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BADGE_LCD_H_RES * FILL_STRIP_LINES * sizeof(uint16_t),
    };
    esp_err_t err = spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = BADGE_LCD_PIN_CS,
        .dc_gpio_num = BADGE_LCD_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = BADGE_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(io, &panel_cfg, &panel);
    if (err != ESP_OK) {
        return err;
    }

    esp_lcd_panel_reset(panel);
    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "panel init failed: %s", esp_err_to_name(err));
        return err;
    }
    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, true, false);
    /* Without this, pure B/W log colors come out inverted (black-on-white). */
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_disp_on_off(panel, true);

    s_fill_buf = heap_caps_malloc((size_t)BADGE_LCD_H_RES * FILL_STRIP_LINES * sizeof(uint16_t),
                                  MALLOC_CAP_DMA);
    if (!s_fill_buf) {
        return ESP_ERR_NO_MEM;
    }

    s_panel = panel;
    s_ready = true;
    s_log_line = 0;
    if (out_panel) {
        *out_panel = panel;
    }
    return ESP_OK;
}

void fl_display_clear(esp_lcd_panel_handle_t panel, uint16_t rgb565)
{
    if (!s_ready || !panel || !s_fill_buf) {
        return;
    }
    uint16_t be = rgb565_be(rgb565);
    for (int y = 0; y < BADGE_LCD_V_RES;) {
        int h = BADGE_LCD_V_RES - y;
        if (h > FILL_STRIP_LINES) {
            h = FILL_STRIP_LINES;
        }
        size_t n = (size_t)BADGE_LCD_H_RES * (size_t)h;
        for (size_t i = 0; i < n; i++) {
            s_fill_buf[i] = be;
        }
        esp_lcd_panel_draw_bitmap(panel, 0, y, BADGE_LCD_H_RES, y + h, s_fill_buf);
        y += h;
    }
    s_log_line = 0;
}

static void draw_char(esp_lcd_panel_handle_t panel, int x, int y, char c, uint16_t fg, uint16_t bg)
{
    unsigned idx = (unsigned)(unsigned char)c;
    if (idx < 0x20 || idx > 0x7F) {
        idx = (unsigned)'?';
    }
    idx -= 0x20;
    uint16_t fg_be = rgb565_be(fg);
    uint16_t bg_be = rgb565_be(bg);
    uint16_t rowbuf[8 * LOG_SCALE];

    for (int row = 0; row < 8; row++) {
        uint8_t bits = fl_font8x8[idx][row];
        for (int sy = 0; sy < LOG_SCALE; sy++) {
            for (int col = 0; col < 8; col++) {
                uint16_t pix = (bits & (1u << col)) ? fg_be : bg_be;
                for (int sx = 0; sx < LOG_SCALE; sx++) {
                    rowbuf[col * LOG_SCALE + sx] = pix;
                }
            }
            esp_lcd_panel_draw_bitmap(panel, x, y + row * LOG_SCALE + sy, x + 8 * LOG_SCALE,
                                      y + row * LOG_SCALE + sy + 1, rowbuf);
        }
    }
}

void fl_display_log(esp_lcd_panel_handle_t panel, const char *line)
{
    if (!line) {
        return;
    }
    ESP_LOGI(TAG, "%s", line);
    if (!s_ready || !panel) {
        return;
    }

    if (s_log_line >= LOG_MAX_LINES) {
        fl_display_clear(panel, LOG_BG);
    }

    int y = 8 + s_log_line * (8 * LOG_SCALE + 4);
    int x = 8;
    for (const char *p = line; *p && x + 8 * LOG_SCALE <= BADGE_LCD_H_RES - 8; p++) {
        draw_char(panel, x, y, *p, LOG_FG, LOG_BG);
        x += 8 * LOG_SCALE;
    }
    s_log_line++;
}
