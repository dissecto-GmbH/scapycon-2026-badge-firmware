#include "usb_mode_cmd.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "radio.h"

static const char *TAG = "usb_cmd";

#define CMD_FOLLOW_TIMEOUT_US 1000000LL

static bool s_await_key;
static int64_t s_prefix_us;

usb_mode_cmd_result_t usb_mode_cmd_feed(uint8_t c, badge_radio_mode_t *out_mode)
{
    if (out_mode) {
        *out_mode = BADGE_RADIO_MODE_BADGE;
    }

    int64_t now = esp_timer_get_time();

    if (!s_await_key) {
        if (c == USB_MODE_CMD_PREFIX) {
            s_await_key = true;
            s_prefix_us = now;
            return USB_MODE_CMD_CONSUMED;
        }
        return USB_MODE_CMD_PASS;
    }

    /* Second key after CTRL+B */
    if ((now - s_prefix_us) > CMD_FOLLOW_TIMEOUT_US) {
        s_await_key = false;
        /* Stale prefix — re-process this byte as a fresh input. */
        return usb_mode_cmd_feed(c, out_mode);
    }

    if (c == USB_MODE_CMD_PREFIX) {
        /* Double CTRL+B: restart prefix wait. */
        s_prefix_us = now;
        return USB_MODE_CMD_CONSUMED;
    }

    s_await_key = false;

    badge_radio_mode_t mode;
    switch (c) {
    case '1':
    case 'a':
    case 'A':
        mode = BADGE_RADIO_MODE_BADGE;
        break;
    case '2':
    case 'b':
    case 'B':
        mode = BADGE_RADIO_MODE_SNIFFER;
        break;
    case '3':
    case 'c':
    case 'C':
        mode = BADGE_RADIO_MODE_SCHEDULE;
        break;
    default:
        return USB_MODE_CMD_CONSUMED;
    }

    if (out_mode) {
        *out_mode = mode;
    }

    esp_err_t err = badge_radio_request_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mode request failed: %s", esp_err_to_name(err));
        return USB_MODE_CMD_CONSUMED;
    }

    static const char *labels[] = {
        [BADGE_RADIO_MODE_SNIFFER] = "sniffer",
        [BADGE_RADIO_MODE_SCHEDULE] = "schedule",
        [BADGE_RADIO_MODE_BADGE] = "badge",
    };
    ESP_LOGI(TAG, "CTRL+B %c → %s", (char)c, labels[mode]);
    return USB_MODE_CMD_MODE;
}
