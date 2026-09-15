#pragma once

#include <stdint.h>

#include "radio.h"

#define USB_MODE_CMD_PREFIX 0x02 /* CTRL+B (tmux-style) */

typedef enum {
    USB_MODE_CMD_PASS = 0,     /* not part of a command — caller handles byte */
    USB_MODE_CMD_CONSUMED = 1, /* prefix / timed-out / invalid follow-up */
    USB_MODE_CMD_MODE = 2,     /* mode change queued; *out_mode set */
} usb_mode_cmd_result_t;

/**
 * Feed one host→device byte.
 * CTRL+B then 1/a → badge, 2/b → wifi, 3/c → sniffer (matches SW_A/B/C).
 */
usb_mode_cmd_result_t usb_mode_cmd_feed(uint8_t c, badge_radio_mode_t *out_mode);
