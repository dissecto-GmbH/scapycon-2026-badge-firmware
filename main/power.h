#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t badge_power_init(void);
void badge_power_start_monitor(void);
bool badge_power_usb_present(void);
