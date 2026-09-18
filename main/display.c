#include "display.h"

#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "power.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "font8x8_basic.h"
#include "user_store.h"
#include "utf8_name.h"

static const char *TAG = "display";

#define LCD_HOST           SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define FILL_STRIP_LINES   40
#define BL_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define BL_LEDC_TIMER      LEDC_TIMER_0
#define BL_LEDC_CHANNEL    LEDC_CHANNEL_0
#define BL_LEDC_RES        LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ_HZ    5000

static uint16_t *s_fill_buf;
static esp_lcd_panel_io_handle_t s_io;
static bool s_ready;
static bool s_ledc_ready;
static uint8_t s_brightness_percent = 0xFF; /* force first apply */

static uint16_t rgb565_be(uint16_t color)
{
    return (color >> 8) | (color << 8);
}

/**
 * SPI color TX is queued (DMA). draw_bitmap returns before the buffer is idle.
 * Wait before reusing s_fill_buf or the next strip/name corrupts in-flight pixels
 * (looks like a shifted second copy of the background over the name).
 */
static void display_wait_idle(void)
{
    if (s_io) {
        (void)esp_lcd_panel_io_tx_param(s_io, LCD_CMD_NOP, NULL, 0);
    }
}

static void draw_bitmap_sync(esp_lcd_panel_handle_t panel, int x0, int y0, int x1, int y1,
                             const void *color)
{
    esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, color);
    display_wait_idle();
}

static void display_cleanup(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel, bool spi_inited)
{
    if (panel) {
        esp_lcd_panel_del(panel);
    }
    if (io) {
        esp_lcd_panel_io_del(io);
    }
    s_io = NULL;
    if (spi_inited) {
        spi_bus_free(LCD_HOST);
    }
    if (s_fill_buf) {
        free(s_fill_buf);
        s_fill_buf = NULL;
    }
    if (s_ledc_ready) {
        ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, 0);
        ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    } else {
        gpio_set_level(BADGE_LCD_PIN_BL, 0);
    }
    s_brightness_percent = 0;
    s_ready = false;
}

void badge_display_set_brightness(uint8_t percent)
{
    if (percent > 100) {
        percent = 100;
    }
    if (percent == s_brightness_percent) {
        return;
    }

    if (s_ledc_ready) {
        uint32_t max_duty = (1u << BL_LEDC_RES) - 1u;
        uint32_t duty = (max_duty * percent) / 100u;
        ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty);
        ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
    } else {
        gpio_set_level(BADGE_LCD_PIN_BL, percent > 0 ? 1 : 0);
    }
    s_brightness_percent = percent;
}

bool badge_display_is_ready(void)
{
    return s_ready;
}

static esp_err_t backlight_pwm_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = BL_LEDC_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        return err;
    }

    ledc_channel_config_t channel = {
        .gpio_num = BADGE_LCD_PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        return err;
    }

    s_ledc_ready = true;
    s_brightness_percent = 0xFF;
    return ESP_OK;
}

esp_err_t badge_display_init(esp_lcd_panel_handle_t *out_panel)
{
    if (out_panel) {
        *out_panel = NULL;
    }
    s_ready = false;
    s_ledc_ready = false;

    esp_err_t err = backlight_pwm_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LEDC backlight failed (%s), using GPIO on/off", esp_err_to_name(err));
        gpio_config_t bl_cfg = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << BADGE_LCD_PIN_BL,
        };
        err = gpio_config(&bl_cfg);
        if (err != ESP_OK) {
            return err;
        }
        gpio_set_level(BADGE_LCD_PIN_BL, 0);
        s_brightness_percent = 0;
    }

    spi_bus_config_t buscfg = {
        .sclk_io_num = BADGE_LCD_PIN_SCLK,
        .mosi_io_num = BADGE_LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = BADGE_LCD_H_RES * 40 * sizeof(uint16_t),
    };
    err = spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI init failed: %s", esp_err_to_name(err));
        return ESP_OK;
    }
    bool spi_inited = (err == ESP_OK);

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = BADGE_LCD_PIN_DC,
        .cs_gpio_num = BADGE_LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    err = esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no display (panel IO): %s", esp_err_to_name(err));
        if (spi_inited) {
            spi_bus_free(LCD_HOST);
        }
        return ESP_OK;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = BADGE_LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    err = esp_lcd_new_panel_st7789(io, &panel_config, &panel);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no display (panel): %s", esp_err_to_name(err));
        display_cleanup(io, NULL, spi_inited);
        return ESP_OK;
    }

    err = esp_lcd_panel_reset(panel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display reset failed: %s", esp_err_to_name(err));
        display_cleanup(io, panel, spi_inited);
        return ESP_OK;
    }

    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "display init failed: %s", esp_err_to_name(err));
        display_cleanup(io, panel, spi_inited);
        return ESP_OK;
    }

    esp_lcd_panel_swap_xy(panel, true);
    esp_lcd_panel_mirror(panel, true, false);
    /* ST7789 on this panel: without invert, framebuffer colors are inverted
     * (white→black, green→magenta) — same as flashloader/fl_display.c. */
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_disp_on_off(panel, true);

    size_t strip_pixels = (size_t)BADGE_LCD_H_RES * FILL_STRIP_LINES;
    s_fill_buf = heap_caps_malloc(strip_pixels * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!s_fill_buf) {
        ESP_LOGE(TAG, "fill strip buffer alloc failed");
        display_cleanup(io, panel, spi_inited);
        return ESP_OK;
    }

    badge_display_set_brightness(badge_power_usb_present() ? BADGE_BACKLIGHT_USB_PERCENT
                                                           : BADGE_BACKLIGHT_BATTERY_PERCENT);
    s_io = io;
    s_ready = true;
    if (out_panel) {
        *out_panel = panel;
    }
    ESP_LOGI(TAG, "ST7789 ready (%dx%d)", BADGE_LCD_H_RES, BADGE_LCD_V_RES);
    return ESP_OK;
}

void badge_display_fill(esp_lcd_panel_handle_t panel, int x, int y, int w, int h, uint16_t rgb565)
{
    if (!s_ready || !panel || w <= 0 || h <= 0 || !s_fill_buf) {
        return;
    }

    uint16_t be = rgb565_be(rgb565);
    int y_end = y + h;

    for (int y_off = y; y_off < y_end;) {
        int strip_h = y_end - y_off;
        if (strip_h > FILL_STRIP_LINES) {
            strip_h = FILL_STRIP_LINES;
        }

        size_t pixels = (size_t)w * (size_t)strip_h;
        for (size_t i = 0; i < pixels; i++) {
            s_fill_buf[i] = be;
        }

        draw_bitmap_sync(panel, x, y_off, x + w, y_off + strip_h, s_fill_buf);
        y_off += strip_h;
    }
}

static bool clip_rect(int *x, int *y, int *w, int *h)
{
    if (*w <= 0 || *h <= 0) {
        return false;
    }
    if (*x >= BADGE_LCD_H_RES || *y >= BADGE_LCD_V_RES) {
        return false;
    }
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > BADGE_LCD_H_RES) {
        *w = BADGE_LCD_H_RES - *x;
    }
    if (*y + *h > BADGE_LCD_V_RES) {
        *h = BADGE_LCD_V_RES - *y;
    }
    return *w > 0 && *h > 0;
}

void badge_display_restore_bg(esp_lcd_panel_handle_t panel, int x, int y, int w, int h,
                              uint16_t fallback_rgb565)
{
    if (!s_ready || !panel || !s_fill_buf) {
        return;
    }
    if (!clip_rect(&x, &y, &w, &h)) {
        return;
    }
    if (!user_store_bg_valid()) {
        badge_display_fill(panel, x, y, w, h, fallback_rgb565);
        return;
    }

    /* Stream stored RGB565 (already panel big-endian) in horizontal strips.
     * Wait for DMA after each strip before refilling s_fill_buf. */
    int y_end = y + h;
    for (int y_off = y; y_off < y_end;) {
        int strip_h = y_end - y_off;
        if (strip_h > FILL_STRIP_LINES) {
            strip_h = FILL_STRIP_LINES;
        }

        if (x == 0 && w == BADGE_LCD_H_RES) {
            size_t off = (size_t)y_off * BADGE_LCD_H_RES * sizeof(uint16_t);
            size_t nbytes = (size_t)strip_h * BADGE_LCD_H_RES * sizeof(uint16_t);
            if (user_store_read_bg(off, s_fill_buf, nbytes) != ESP_OK) {
                badge_display_fill(panel, x, y, w, h, fallback_rgb565);
                return;
            }
        } else {
            for (int row = 0; row < strip_h; row++) {
                size_t off = ((size_t)(y_off + row) * BADGE_LCD_H_RES + (size_t)x) * sizeof(uint16_t);
                size_t nbytes = (size_t)w * sizeof(uint16_t);
                if (user_store_read_bg(off, (uint8_t *)s_fill_buf + (size_t)row * nbytes, nbytes) !=
                    ESP_OK) {
                    badge_display_fill(panel, x, y, w, h, fallback_rgb565);
                    return;
                }
            }
        }

        draw_bitmap_sync(panel, x, y_off, x + w, y_off + strip_h, s_fill_buf);
        y_off += strip_h;
    }
}

void badge_display_clear(esp_lcd_panel_handle_t panel, uint16_t fallback_rgb565)
{
    badge_display_restore_bg(panel, 0, 0, BADGE_LCD_H_RES, BADGE_LCD_V_RES, fallback_rgb565);
}

static void draw_glyph(esp_lcd_panel_handle_t panel, int x, int y, unsigned idx,
                       uint16_t fg, uint16_t bg, int scale, bool transparent)
{
    if (!s_ready || !panel || !s_fill_buf || scale < 1) {
        return;
    }
    if (idx >= UTF8_GLYPH_COUNT) {
        idx = (unsigned)'?';
    }

    const uint8_t *glyph = font8x8_basic[idx];
    int w = 8 * scale;
    uint16_t fg_be = rgb565_be(fg);
    uint16_t bg_be = rgb565_be(bg);
    bool use_bg_img = transparent && user_store_bg_valid();

    for (int row = 0; row < 8; row++) {
        for (int sy = 0; sy < scale; sy++) {
            int py = y + row * scale + sy;

            if (use_bg_img && py >= 0 && py < BADGE_LCD_V_RES && x >= 0 &&
                x + w <= BADGE_LCD_H_RES) {
                size_t off = ((size_t)py * BADGE_LCD_H_RES + (size_t)x) * sizeof(uint16_t);
                if (user_store_read_bg(off, s_fill_buf, (size_t)w * sizeof(uint16_t)) != ESP_OK) {
                    use_bg_img = false;
                }
            }

            if (!use_bg_img) {
                for (int col = 0; col < 8; col++) {
                    uint16_t color = (glyph[row] & (1 << col)) ? fg_be : bg_be;
                    for (int sx = 0; sx < scale; sx++) {
                        s_fill_buf[col * scale + sx] = color;
                    }
                }
            } else {
                for (int col = 0; col < 8; col++) {
                    if (glyph[row] & (1 << col)) {
                        for (int sx = 0; sx < scale; sx++) {
                            s_fill_buf[col * scale + sx] = fg_be;
                        }
                    }
                    /* else: leave RGB565 from user_store_read_bg */
                }
            }

            draw_bitmap_sync(panel, x, py, x + w, py + 1, s_fill_buf);
        }
    }
}

void badge_display_draw_char(esp_lcd_panel_handle_t panel, int x, int y, char c,
                             uint16_t fg, uint16_t bg, int scale)
{
    draw_glyph(panel, x, y, utf8_codepoint_to_glyph((unsigned char)c), fg, bg, scale, false);
}

int badge_display_draw_string(esp_lcd_panel_handle_t panel, int x, int y, const char *str,
                              uint16_t fg, uint16_t bg, int scale)
{
    if (!s_ready || !panel || !str || scale < 1) {
        return 0;
    }

    int cx = x;
    while (*str) {
        uint32_t cp;
        size_t nb;
        if (!utf8_next(str, &cp, &nb)) {
            str++;
            continue;
        }
        unsigned idx = utf8_codepoint_to_glyph(cp);
        draw_glyph(panel, cx, y, idx, fg, bg, scale, false);
        cx += 8 * scale + 1;
        str += nb;
    }
    return cx - x;
}

int badge_display_draw_string_transparent(esp_lcd_panel_handle_t panel, int x, int y, const char *str,
                                          uint16_t fg, uint16_t fallback_bg, int scale)
{
    if (!s_ready || !panel || !str || scale < 1) {
        return 0;
    }

    int cx = x;
    while (*str) {
        uint32_t cp;
        size_t nb;
        if (!utf8_next(str, &cp, &nb)) {
            str++;
            continue;
        }
        unsigned idx = utf8_codepoint_to_glyph(cp);
        draw_glyph(panel, cx, y, idx, fg, fallback_bg, scale, true);
        cx += 8 * scale + 1;
        str += nb;
    }
    return cx - x;
}

void badge_display_draw_line(esp_lcd_panel_handle_t panel, int x, int y, int w, const char *str,
                             uint16_t fg, uint16_t bg, int scale)
{
    if (!s_ready || !panel || !str || scale < 1 || w <= 0) {
        return;
    }

    int h = 8 * scale;
    badge_display_fill(panel, x, y, w, h, bg);
    badge_display_draw_string(panel, x, y, str, fg, bg, scale);
}

void badge_display_blit_mono(esp_lcd_panel_handle_t panel, const uint8_t *mono, int stride_bytes,
                             int y0, int y1, const badge_mono_color_band_t *bands, size_t nbands,
                             uint16_t default_fg, uint16_t default_bg)
{
    if (!s_ready || !panel || !mono || stride_bytes <= 0 || !s_fill_buf) {
        return;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (y1 > BADGE_LCD_V_RES) {
        y1 = BADGE_LCD_V_RES;
    }
    if (y0 >= y1) {
        return;
    }

    uint16_t def_fg = rgb565_be(default_fg);
    uint16_t def_bg = rgb565_be(default_bg);

    for (int y = y0; y < y1;) {
        int strip_h = y1 - y;
        if (strip_h > FILL_STRIP_LINES) {
            strip_h = FILL_STRIP_LINES;
        }

        for (int row = 0; row < strip_h; row++) {
            int py = y + row;
            uint16_t fg = def_fg;
            uint16_t bg = def_bg;
            if (bands) {
                for (size_t i = 0; i < nbands; i++) {
                    if (py >= bands[i].y0 && py < bands[i].y1) {
                        fg = rgb565_be(bands[i].fg);
                        bg = rgb565_be(bands[i].bg);
                        break;
                    }
                }
            }

            const uint8_t *src = mono + (size_t)py * (size_t)stride_bytes;
            uint16_t *dst = s_fill_buf + (size_t)row * (size_t)BADGE_LCD_H_RES;
            for (int x = 0; x < BADGE_LCD_H_RES; x++) {
                uint8_t bit = (uint8_t)((src[x >> 3] >> (x & 7)) & 1u);
                dst[x] = bit ? fg : bg;
            }
        }

        draw_bitmap_sync(panel, 0, y, BADGE_LCD_H_RES, y + strip_h, s_fill_buf);
        y += strip_h;
    }
}
