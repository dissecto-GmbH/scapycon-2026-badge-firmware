#include "badge_setup.h"

#include <string.h>

#include "badge_name.h"
#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "host_link.h"
#include "user_store.h"
#include "utf8_name.h"
#include "usb_mode_cmd.h"

static const char *TAG = "badge_setup";

static TaskHandle_t s_task;
static volatile bool s_running;
static bool s_host_up;
static int s_idle_polls;
static int64_t s_last_tx_us;

#define USB_TX_FLUSH_TIMEOUT_US 50000
#define HOST_POLL_MS            50
#define HOST_DETACH_POLLS       20

#define SETUP_LINE_MAX USER_STORE_PASS_MAX

typedef enum {
    SETUP_MENU = 0,
    SETUP_EDIT_NAME,
    SETUP_EDIT_SSID,
    SETUP_EDIT_PASS,
} setup_state_t;

static setup_state_t s_state;

static bool usb_ready(void)
{
    return usb_serial_jtag_is_driver_installed();
}

static void usb_ll_flush_zlp(void)
{
    usb_serial_jtag_ll_txfifo_flush();
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < USB_TX_FLUSH_TIMEOUT_US) {
        if (usb_serial_jtag_ll_txfifo_writable()) {
            usb_serial_jtag_ll_txfifo_flush();
            s_last_tx_us = esp_timer_get_time();
            return;
        }
        vTaskDelay(1);
    }
}

static void usb_out(const char *s)
{
    if (!s || !*s || !usb_ready() || !s_host_up) {
        return;
    }

    /* Keep the driver TX ISR from racing the FIFO while we write. */
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_SERIAL_IN_EMPTY);

    int chunk = 0;
    for (const char *p = s; *p; p++) {
        uint8_t c = (uint8_t)*p;
        int64_t deadline = esp_timer_get_time() + USB_TX_FLUSH_TIMEOUT_US;
        while (!usb_serial_jtag_ll_txfifo_writable()) {
            if (esp_timer_get_time() >= deadline) {
                ESP_LOGW(TAG, "usb LL TX fifo full");
                return;
            }
            vTaskDelay(1);
        }
        usb_serial_jtag_ll_write_txfifo(&c, 1);
        chunk++;
        if (c == '\n' || chunk >= 60) {
            usb_ll_flush_zlp();
            chunk = 0;
        }
    }
    if (chunk > 0) {
        usb_ll_flush_zlp();
    }
}

static void print_menu(void)
{
    char ssid[USER_STORE_SSID_MAX];
    char pass[USER_STORE_PASS_MAX];
    bool wifi_ok = user_store_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK;

    s_state = SETUP_MENU;
    usb_out("\r\n=== Scapycon 2026 Badge ===\r\n");
    usb_out("Setup console — choose an option:\r\n");
    usb_out("  1  Name          [");
    usb_out(badge_name_get());
    usb_out("]\r\n");
    usb_out("  2  Wi-Fi SSID    [");
    usb_out(wifi_ok ? ssid : "(not set)");
    usb_out("]\r\n");
    usb_out("  3  Wi-Fi key     [");
    usb_out(wifi_ok && pass[0] ? "set" : "empty");
    usb_out("]\r\n");
    usb_out("Enter 1-3: ");
}

static void print_edit_prompt(void)
{
    switch (s_state) {
    case SETUP_EDIT_NAME:
        usb_out("\r\nNew name (Enter cancels):\r\n> ");
        break;
    case SETUP_EDIT_SSID:
        usb_out("\r\nNew Wi-Fi SSID (Enter cancels):\r\n> ");
        break;
    case SETUP_EDIT_PASS:
        usb_out("\r\nNew Wi-Fi key (Enter cancels, input hidden):\r\n> ");
        break;
    default:
        print_menu();
        break;
    }
}

static bool read_char(uint8_t *out, TickType_t timeout)
{
    if (!usb_ready()) {
        return false;
    }
    return usb_serial_jtag_read_bytes(out, 1, timeout) > 0;
}

/* Poll only — never enable TOKEN_REC_IN_EP1 in int_ena (that caused IWDT storms). */
static bool host_in_token_seen(void)
{
    uint32_t raw = usb_serial_jtag_ll_get_intraw_mask();
    if (raw & USB_SERIAL_JTAG_INTR_TOKEN_REC_IN_EP1) {
        usb_serial_jtag_ll_clr_intsts_mask(USB_SERIAL_JTAG_INTR_TOKEN_REC_IN_EP1);
        return true;
    }
    return false;
}

static void on_host_opened(void)
{
    ESP_LOGI(TAG, "USB serial host ready");
    s_host_up = true;
    s_idle_polls = 0;
    /* Host is polling EP IN — safe to TX. */
    vTaskDelay(pdMS_TO_TICKS(30));
    print_menu();
}

static void poll_host_attach(void)
{
    bool token = host_in_token_seen();

    if (!s_host_up) {
        if (token) {
            on_host_opened();
        }
        return;
    }

    if (token) {
        s_idle_polls = 0;
        return;
    }

    /* Ignore brief gaps right after we transmitted. */
    if ((esp_timer_get_time() - s_last_tx_us) < 500000) {
        return;
    }

    s_idle_polls++;
    if (s_idle_polls >= HOST_DETACH_POLLS) {
        ESP_LOGI(TAG, "USB serial host closed");
        s_host_up = false;
        s_idle_polls = 0;
    }
}

static esp_err_t wifi_update_field(bool set_ssid, const char *value)
{
    char ssid[USER_STORE_SSID_MAX];
    char pass[USER_STORE_PASS_MAX];
    ssid[0] = '\0';
    pass[0] = '\0';

    const user_store_header_t *hdr = user_store_header();
    if (hdr) {
        strlcpy(ssid, hdr->wifi_ssid, sizeof(ssid));
        strlcpy(pass, hdr->wifi_pass, sizeof(pass));
    }

    if (set_ssid) {
        strlcpy(ssid, value, sizeof(ssid));
    } else {
        strlcpy(pass, value, sizeof(pass));
    }

    if (ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    return user_store_set_wifi(ssid, pass);
}

static void handle_menu_line(const char *line)
{
    if (strcmp(line, "1") == 0) {
        s_state = SETUP_EDIT_NAME;
        print_edit_prompt();
        return;
    }
    if (strcmp(line, "2") == 0) {
        s_state = SETUP_EDIT_SSID;
        print_edit_prompt();
        return;
    }
    if (strcmp(line, "3") == 0) {
        s_state = SETUP_EDIT_PASS;
        print_edit_prompt();
        return;
    }
    usb_out("Unknown option. Enter 1, 2, or 3.\r\n");
    print_menu();
}

static void handle_edit_line(const char *line)
{
    setup_state_t done = s_state;
    esp_err_t err = ESP_OK;

    if (!line[0]) {
        usb_out("Cancelled.\r\n");
        print_menu();
        return;
    }

    switch (done) {
    case SETUP_EDIT_NAME:
        err = badge_name_set(line);
        if (err == ESP_OK) {
            err = user_store_set_name(badge_name_get());
        }
        if (err == ESP_OK) {
            usb_out("Saved name: ");
            usb_out(badge_name_get());
            usb_out("\r\n");
        } else if (err == ESP_ERR_INVALID_ARG) {
            usb_out("Name cannot be empty.\r\n");
        } else {
            usb_out("Could not save name.\r\n");
        }
        break;

    case SETUP_EDIT_SSID:
        err = wifi_update_field(true, line);
        if (err == ESP_OK) {
            usb_out("Saved Wi-Fi SSID: ");
            usb_out(line);
            usb_out("\r\n");
        } else if (err == ESP_ERR_INVALID_ARG) {
            usb_out("SSID cannot be empty.\r\n");
        } else {
            usb_out("Could not save SSID.\r\n");
        }
        break;

    case SETUP_EDIT_PASS:
        err = wifi_update_field(false, line);
        if (err == ESP_OK) {
            usb_out("Saved Wi-Fi key.\r\n");
        } else if (err == ESP_ERR_INVALID_ARG) {
            usb_out("Set an SSID first (option 2).\r\n");
        } else {
            usb_out("Could not save Wi-Fi key.\r\n");
        }
        break;

    default:
        break;
    }

    print_menu();
}

static size_t edit_max_len(void)
{
    switch (s_state) {
    case SETUP_EDIT_NAME:
        return BADGE_NAME_MAX;
    case SETUP_EDIT_SSID:
        return USER_STORE_SSID_MAX - 1;
    case SETUP_EDIT_PASS:
        return USER_STORE_PASS_MAX - 1;
    default:
        return 8;
    }
}

static bool edit_allows_utf8(void)
{
    return s_state == SETUP_EDIT_NAME;
}

static void echo_char(char c, bool hide)
{
    char echo[2] = {hide ? '*' : c, '\0'};
    usb_out(echo);
}

static void setup_task(void *arg)
{
    (void)arg;

    /* Defer driver install until boot logging is done — installing while the
     * host already has /dev/ttyACM0 open wedges esp_intr_alloc on C5. */
    vTaskDelay(pdMS_TO_TICKS(1500));

    for (int i = 0; i < 20 && s_running && !usb_ready(); i++) {
        if (badge_usb_ensure_driver() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (!usb_ready()) {
        ESP_LOGE(TAG, "USB driver not ready");
    }

    /* Ensure TOKEN is not interrupt-enabled (poll-only). */
    usb_serial_jtag_ll_disable_intr_mask(USB_SERIAL_JTAG_INTR_TOKEN_REC_IN_EP1);

    s_host_up = false;
    s_idle_polls = 0;
    s_state = SETUP_MENU;

    char line[SETUP_LINE_MAX + 1];
    size_t line_len = 0;
    uint8_t utf8_lead = 0;

    while (s_running) {
        if (!usb_ready()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        poll_host_attach();

        uint8_t c;
        if (!read_char(&c, pdMS_TO_TICKS(HOST_POLL_MS))) {
            continue;
        }

        badge_radio_mode_t cmd_mode;
        usb_mode_cmd_result_t cmd = usb_mode_cmd_feed(c, &cmd_mode);
        if (cmd != USB_MODE_CMD_PASS) {
            utf8_lead = 0;
            if (cmd == USB_MODE_CMD_MODE) {
                static const char *labels[] = {
                    [BADGE_RADIO_MODE_SNIFFER] = "sniffer",
                    [BADGE_RADIO_MODE_SCHEDULE] = "schedule",
                    [BADGE_RADIO_MODE_BADGE] = "badge",
                };
                usb_out("\r\nMode → ");
                usb_out(labels[cmd_mode]);
                usb_out("\r\n");
            }
            continue;
        }

        if (!s_host_up) {
            on_host_opened();
            if (c == '\r' || c == '\n') {
                line_len = 0;
                utf8_lead = 0;
                continue;
            }
        }

        if (c == '\r' || c == '\n') {
            utf8_lead = 0;
            line[line_len] = '\0';

            if (s_state == SETUP_MENU) {
                if (line_len > 0) {
                    handle_menu_line(line);
                } else {
                    print_menu();
                }
            } else {
                handle_edit_line(line);
            }

            line_len = 0;
            continue;
        }

        if (c == 0x08 || c == 0x7f) {
            utf8_lead = 0;
            if (line_len > 0) {
                size_t drop = utf8_prev_char_len(line, line_len);
                line_len -= drop;
                usb_out("\b \b");
            }
            continue;
        }

        size_t max_len = edit_max_len();
        bool hide = (s_state == SETUP_EDIT_PASS);

        /* Complete a pending 2-byte UTF-8 sequence (German letters are C3 xx). */
        if (utf8_lead) {
            char pair[3] = {(char)utf8_lead, (char)c, '\0'};
            utf8_lead = 0;

            if (!edit_allows_utf8() || s_state == SETUP_MENU) {
                continue;
            }

            uint32_t cp;
            size_t nb;
            if (utf8_next(pair, &cp, &nb) && nb == 2 && utf8_name_codepoint_allowed(cp) &&
                line_len + 2 <= max_len && line_len + 2 < sizeof(line)) {
                line[line_len++] = pair[0];
                line[line_len++] = pair[1];
                usb_out(pair);
            }
            continue;
        }

        if (c >= 32 && c < 127 && line_len + 1 <= max_len && line_len + 1 < sizeof(line)) {
            line[line_len++] = (char)c;
            echo_char((char)c, hide);
            continue;
        }

        /* Start of 2-byte UTF-8 (C2/C3); German allowlist uses C3. */
        if (edit_allows_utf8() && (c == 0xC2 || c == 0xC3) && line_len + 2 <= max_len &&
            line_len + 2 < sizeof(line)) {
            utf8_lead = c;
        }
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t badge_setup_start(void)
{
    if (s_task) {
        return ESP_OK;
    }

    s_running = true;
    BaseType_t ok = xTaskCreate(setup_task, "badge_setup", 4096, NULL, 5, &s_task);
    if (ok != pdPASS) {
        s_running = false;
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "USB setup console active");
    return ESP_OK;
}

void badge_setup_stop(void)
{
    if (!s_task) {
        return;
    }

    s_running = false;
    for (int i = 0; i < 20 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_task) {
        vTaskDelete(s_task);
        s_task = NULL;
    }

    ESP_LOGI(TAG, "USB setup console stopped");
}
