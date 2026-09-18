#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

esp_err_t badge_display_init(esp_lcd_panel_handle_t *out_panel);
bool badge_display_is_ready(void);
void badge_display_fill(esp_lcd_panel_handle_t panel, int x, int y, int w, int h, uint16_t rgb565);

/** Fill with solid color, or stream RGB565 from user partition if a bg is present. */
void badge_display_clear(esp_lcd_panel_handle_t panel, uint16_t fallback_rgb565);

/** Restore a rectangle from the user-store background (or solid fallback). */
void badge_display_restore_bg(esp_lcd_panel_handle_t panel, int x, int y, int w, int h,
                              uint16_t fallback_rgb565);

void badge_display_draw_char(esp_lcd_panel_handle_t panel, int x, int y, char c,
                             uint16_t fg, uint16_t bg, int scale);
int badge_display_draw_string(esp_lcd_panel_handle_t panel, int x, int y, const char *str,
                              uint16_t fg, uint16_t bg, int scale);
/** Glyph background samples the user-store RGB565 image (falls back to solid if none). */
int badge_display_draw_string_transparent(esp_lcd_panel_handle_t panel, int x, int y, const char *str,
                                          uint16_t fg, uint16_t fallback_bg, int scale);
void badge_display_draw_line(esp_lcd_panel_handle_t panel, int x, int y, int w, const char *str,
                             uint16_t fg, uint16_t bg, int scale);

/**
 * Color band for badge_display_blit_mono: rows [y0, y1) use fg/bg.
 * First matching band wins; rows outside all bands use the blit defaults.
 */
typedef struct {
    int y0;
    int y1;
    uint16_t fg;
    uint16_t bg;
} badge_mono_color_band_t;

/**
 * Push rows [y0, y1) of a 1-bit framebuffer (1 = ink) to the panel.
 * mono is row-major, stride_bytes bytes per row, LSB = leftmost pixel in each byte.
 */
void badge_display_blit_mono(esp_lcd_panel_handle_t panel, const uint8_t *mono, int stride_bytes,
                             int y0, int y1, const badge_mono_color_band_t *bands, size_t nbands,
                             uint16_t default_fg, uint16_t default_bg);

void badge_display_set_brightness(uint8_t percent);
