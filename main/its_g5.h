#pragma once

#include <stdint.h>

#include "esp_err.h"

/** Default ITS-G5 control channel (G5CC) frequency in MHz. */
#define BADGE_ITS_G5_CHANNEL_MHZ 5900

esp_err_t badge_its_g5_sniffer_start(void);
esp_err_t badge_its_g5_sniffer_stop(void);
uint32_t badge_its_g5_packet_count(void);

/**
 * Copy the most recent RX frame into out (truncated to max_len).
 * Safe to call from the UI task; never call from the promiscuous RX callback.
 * Returns the number of bytes copied (0 if none yet).
 */
uint16_t badge_its_g5_copy_last_packet(uint8_t *out, uint16_t max_len);

/** Currently selected ITS frequency in MHz (updated by cycle while sniffing). */
int badge_its_g5_get_channel_mhz(void);

/**
 * Advance to the next ITS-G5 channel (5860…5920). Safe to call from the radio
 * task; applies phy_change_channel immediately if the sniffer is running.
 */
int badge_its_g5_cycle_channel(void);

void badge_its_g5_apply_phy(void);
void badge_its_g5_disable_phy(void);
