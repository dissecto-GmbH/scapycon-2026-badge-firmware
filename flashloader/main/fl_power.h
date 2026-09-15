#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** GPIO only — safe before LCD; use for early SW_D check. */
esp_err_t fl_gpio_init_sw_d(void);

/** ADC for VBUS detect — call only on the SW_D-held update path. */
esp_err_t fl_power_adc_init(void);

esp_err_t fl_power_init(void);
bool fl_power_usb_present(void);
bool fl_sw_d_held(void);
