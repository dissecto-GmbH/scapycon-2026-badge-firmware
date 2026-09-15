#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t badge_leds_init(void);
void badge_leds_set(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void badge_leds_refresh(void);
