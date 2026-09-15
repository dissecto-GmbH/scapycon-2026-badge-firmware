#include "its_g5.h"

#include <inttypes.h>
#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host_link.h"

static const char *TAG = "its_g5";

extern void phy_11p_set(int enable, int unknown);
extern void phy_change_channel(int channel, int a, int b, int c);

#define FRAME_QUEUE_LEN 32

typedef struct {
    uint16_t length;
    int8_t rssi;
    uint8_t channel;
    uint8_t data[512];
} its_g5_frame_t;

static QueueHandle_t s_frame_queue;
static TaskHandle_t s_export_task;
static uint32_t s_packet_count;
static bool s_running;
static int s_channel_mhz = BADGE_ITS_G5_CHANNEL_MHZ;

/* ETSI ITS-G5 5.9 GHz channels (10 MHz), G5CC = 5900. */
static const int s_its_channels_mhz[] = {5860, 5870, 5880, 5890, 5900, 5910, 5920};

int badge_its_g5_get_channel_mhz(void)
{
    return s_channel_mhz;
}

static const char *channel_label(int mhz)
{
    return (mhz == 5900) ? "G5CC" : "G5SC";
}

int badge_its_g5_cycle_channel(void)
{
    const int n = (int)(sizeof(s_its_channels_mhz) / sizeof(s_its_channels_mhz[0]));
    int idx = 0;
    for (int i = 0; i < n; i++) {
        if (s_its_channels_mhz[i] == s_channel_mhz) {
            idx = (i + 1) % n;
            break;
        }
    }
    s_channel_mhz = s_its_channels_mhz[idx];

    if (s_running) {
        phy_change_channel(s_channel_mhz, 1, 0, 0);
        esp_err_t err = esp_wifi_set_channel(140, WIFI_SECOND_CHAN_NONE);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "bootstrap ch140: %s", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "channel → %d MHz (%s)", s_channel_mhz, channel_label(s_channel_mhz));
    return s_channel_mhz;
}

void badge_its_g5_apply_phy(void)
{
    phy_11p_set(1, 0);
    phy_change_channel(s_channel_mhz, 1, 0, 0);

    esp_err_t err = esp_wifi_set_channel(140, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "bootstrap ch140: %s (phy channel set)", esp_err_to_name(err));
    }
}

void badge_its_g5_disable_phy(void)
{
    phy_11p_set(0, 0);
}

static void promiscuous_rx_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s_running || type == WIFI_PKT_MISC) {
        return;
    }

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    if (pkt->rx_ctrl.rx_state != 0) {
        return;
    }

    uint16_t len = pkt->rx_ctrl.dump_len;
    if (len == 0 || len > sizeof(((its_g5_frame_t *)0)->data)) {
        return;
    }

    its_g5_frame_t frame = {
        .length = len,
        .rssi = pkt->rx_ctrl.rssi,
        .channel = pkt->rx_ctrl.channel,
    };
    memcpy(frame.data, pkt->payload, len);

    if (xQueueSend(s_frame_queue, &frame, 0) != pdTRUE) {
        /* drop */
    }
}

static void export_task(void *arg)
{
    (void)arg;
    its_g5_frame_t frame;
    while (true) {
        if (xQueueReceive(s_frame_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        s_packet_count++;
        badge_host_link_send_rx_frame(frame.data, frame.length, frame.rssi, frame.channel);
        ESP_LOGD(TAG, "#%" PRIu32 " rssi=%d len=%u", s_packet_count, frame.rssi, frame.length);
    }
}

esp_err_t badge_its_g5_sniffer_start(void)
{
    if (!s_frame_queue) {
        s_frame_queue = xQueueCreate(FRAME_QUEUE_LEN, sizeof(its_g5_frame_t));
        if (!s_frame_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_export_task) {
        xTaskCreate(export_task, "its_export", 4096, NULL, 6, &s_export_task);
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_NULL));

    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL & ~WIFI_PROMIS_FILTER_MASK_FCSFAIL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promiscuous_rx_cb));

    ESP_ERROR_CHECK(esp_wifi_start());

    esp_err_t err = esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_band_mode: %s", esp_err_to_name(err));
    }

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    s_running = true;
    badge_its_g5_apply_phy();

    ESP_LOGI(TAG, "sniffer on %d MHz (%s)", s_channel_mhz, channel_label(s_channel_mhz));
    return ESP_OK;
}

esp_err_t badge_its_g5_sniffer_stop(void)
{
    s_running = false;
    esp_wifi_set_promiscuous_rx_cb(NULL);
    if (esp_wifi_set_promiscuous(false) != ESP_OK) {
        /* wifi may already be stopped during mode switch */
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    badge_its_g5_disable_phy();
    ESP_LOGI(TAG, "sniffer stopped");
    return ESP_OK;
}

uint32_t badge_its_g5_packet_count(void)
{
    return s_packet_count;
}
