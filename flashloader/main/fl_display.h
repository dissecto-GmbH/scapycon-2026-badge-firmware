#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"

esp_err_t fl_display_init(esp_lcd_panel_handle_t *out_panel);
bool fl_display_ready(void);
void fl_display_clear(esp_lcd_panel_handle_t panel, uint16_t rgb565);
void fl_display_log(esp_lcd_panel_handle_t panel, const char *line);
void fl_display_set_brightness(uint8_t percent);
