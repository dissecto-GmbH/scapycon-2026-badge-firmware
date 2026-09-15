#include "host_link.h"

#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_private/rtc_clk.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "usb_mode_cmd.h"

static const char *TAG = "host_link";

/* Outer framing matches esp32-its-tap (0xBEEF + LE16 length). Payload is badge-specific. */
#define HOST_MAGIC_0 0xBE
#define HOST_MAGIC_1 0xEF

#define HOST_MSG_RX_FRAME   0x01
#define HOST_MSG_HEARTBEAT  0x02
#define HOST_MSG_MODE       0x03

#define HOST_MAX_FRAME  512

static SemaphoreHandle_t s_tx_lock;
static TaskHandle_t s_hb_task;
static TaskHandle_t s_cmd_task;
static volatile bool s_host_active;
static volatile bool s_cmd_task_run;
static bool s_bbpll_held;

static void usb_hold_phy(void)
{
    if (!s_bbpll_held) {
        rtc_clk_bbpll_add_consumer();
        s_bbpll_held = true;
    }
}

static esp_err_t host_write_all(const uint8_t *data, size_t len)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t off = 0;
    while (off < len) {
        int n = usb_serial_jtag_write_bytes(data + off, len - off, pdMS_TO_TICKS(100));
        if (n < 0) {
            return ESP_FAIL;
        }
        if (n == 0) {
            return ESP_ERR_TIMEOUT;
        }
        off += (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t host_send_payload(const uint8_t *payload, uint16_t plen)
{
    if (!s_host_active || !s_tx_lock || plen == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t hdr[4] = {HOST_MAGIC_0, HOST_MAGIC_1, (uint8_t)(plen & 0xFF), (uint8_t)(plen >> 8)};
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = host_write_all(hdr, sizeof(hdr));
    if (err == ESP_OK) {
        err = host_write_all(payload, plen);
    }
    xSemaphoreGive(s_tx_lock);
    return err;
}

static void cmd_rx_task(void *arg)
{
    (void)arg;
    uint8_t c;
    while (s_cmd_task_run) {
        if (!s_host_active || !usb_serial_jtag_is_driver_installed()) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(50));
        if (n != 1) {
            continue;
        }
        (void)usb_mode_cmd_feed(c, NULL);
    }
    s_cmd_task = NULL;
    vTaskDelete(NULL);
}

static void heartbeat_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        if (!s_host_active) {
            continue;
        }

        uint8_t pl[1 + 4 + 4 + 1];
        size_t n = 0;
        pl[n++] = HOST_MSG_HEARTBEAT;
        uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000);
        memcpy(pl + n, &uptime, 4);
        n += 4;
        uint32_t rx = badge_radio_rx_count();
        memcpy(pl + n, &rx, 4);
        n += 4;
        pl[n++] = (uint8_t)badge_radio_get_mode();
        host_send_payload(pl, (uint16_t)n);
    }
}

esp_err_t badge_usb_ensure_driver(void)
{
    if (usb_serial_jtag_is_driver_installed()) {
        return ESP_OK;
    }

    /* C5 USB PHY depends on BBPLL; keep it up for CDC RX/TX. */
    usb_hold_phy();

    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 1024;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "usb driver install: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "USB serial driver ready");
    return ESP_OK;
}

void badge_host_link_set_active(bool active)
{
    if (active) {
        (void)badge_usb_ensure_driver();
        s_cmd_task_run = true;
        if (!s_cmd_task) {
            xTaskCreate(cmd_rx_task, "host_cmd", 3072, NULL, 4, &s_cmd_task);
        }
    } else {
        s_cmd_task_run = false;
    }
    s_host_active = active;
}

esp_err_t badge_host_link_init(void)
{
    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) {
        return ESP_ERR_NO_MEM;
    }
    xTaskCreate(heartbeat_task, "host_hb", 3072, NULL, 3, &s_hb_task);
    ESP_LOGI(TAG, "USB host framing ready (magic 0xBEEF)");
    return ESP_OK;
}

void badge_host_link_send_rx_frame(const uint8_t *data, uint16_t len, int8_t rssi, uint8_t channel)
{
    if (!data || len == 0 || len > HOST_MAX_FRAME) {
        return;
    }

    uint8_t pl[1 + 8 + 1 + 1 + 2 + HOST_MAX_FRAME];
    size_t n = 0;
    pl[n++] = HOST_MSG_RX_FRAME;

    uint64_t ts = (uint64_t)esp_timer_get_time();
    memcpy(pl + n, &ts, 8);
    n += 8;

    pl[n++] = (uint8_t)rssi;
    pl[n++] = channel;
    memcpy(pl + n, &len, 2);
    n += 2;
    memcpy(pl + n, data, len);
    n += len;

    if (host_send_payload(pl, (uint16_t)n) != ESP_OK) {
        ESP_LOGD(TAG, "host TX drop len=%u", len);
    }
}

void badge_host_link_send_mode(badge_radio_mode_t mode)
{
    uint8_t pl[2] = {HOST_MSG_MODE, (uint8_t)mode};
    host_send_payload(pl, sizeof(pl));
}
