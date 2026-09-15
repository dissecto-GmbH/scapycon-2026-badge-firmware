#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    BADGE_RADIO_MODE_SNIFFER = 0,
    BADGE_RADIO_MODE_SCHEDULE = 1, /* was Wi-Fi station; radio off, shows agenda */
    BADGE_RADIO_MODE_BADGE = 2,
} badge_radio_mode_t;

esp_err_t badge_radio_init(void);
esp_err_t badge_radio_set_mode(badge_radio_mode_t mode);
/** Queue a mode change (safe from button callbacks / Wi-Fi RX context). */
esp_err_t badge_radio_request_mode(badge_radio_mode_t mode);
/** Queue an ITS-G5 channel cycle (no-op unless currently sniffing). */
esp_err_t badge_radio_request_cycle_channel(void);
/** Queue a brief STA+NTP sync, then stop the radio (skipped while sniffing). */
esp_err_t badge_radio_request_ntp_sync(void);
badge_radio_mode_t badge_radio_get_mode(void);
bool badge_radio_is_badge_mode(void);
void badge_radio_refresh_power_profile(void);

uint32_t badge_radio_rx_count(void);
/** Current sniffer frequency in MHz (meaningful in sniffer mode). */
int badge_radio_its_channel_mhz(void);
const char *badge_radio_get_ip(void);
