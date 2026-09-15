#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "radio.h"

esp_err_t badge_host_link_init(void);
esp_err_t badge_usb_ensure_driver(void);
void badge_host_link_set_active(bool active);
void badge_host_link_send_rx_frame(const uint8_t *data, uint16_t len, int8_t rssi, uint8_t channel);
void badge_host_link_send_mode(badge_radio_mode_t mode);
