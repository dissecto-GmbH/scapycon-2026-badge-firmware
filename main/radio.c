#include "radio.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "badge_name.h"
#include "badge_setup.h"
#include "host_link.h"
#include "its_g5.h"
#include "nvs_flash.h"
#include "time_sync.h"
#include "wifi.h"

static const char *TAG = "radio";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define NTP_CONNECT_TIMEOUT_MS 20000
#define NTP_SYNC_TIMEOUT_MS    15000
#define NTP_MAX_RETRY          8

typedef enum {
    RADIO_CMD_SET_MODE = 0,
    RADIO_CMD_CYCLE_CHANNEL,
    RADIO_CMD_NTP_SYNC,
} radio_cmd_type_t;

typedef struct {
    radio_cmd_type_t type;
    badge_radio_mode_t mode;
} radio_cmd_t;

static badge_radio_mode_t s_mode = BADGE_RADIO_MODE_BADGE;
static QueueHandle_t s_cmd_queue;
static bool s_driver_inited;
static bool s_radio_started;
static bool s_sta_netif_created;
static bool s_ip_handler_registered;
static bool s_ntp_session;
static int s_ntp_retry;
static EventGroupHandle_t s_wifi_events;

static void radio_stop_wifi(void)
{
    if (!s_driver_inited) {
        return;
    }
    if (s_mode == BADGE_RADIO_MODE_SNIFFER && s_radio_started) {
        badge_its_g5_sniffer_stop();
    }
    esp_err_t stop_err = esp_wifi_stop();
    if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(stop_err));
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    s_radio_started = false;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_WIFI_READY) {
        if (s_mode == BADGE_RADIO_MODE_SNIFFER && !s_ntp_session) {
            badge_its_g5_apply_phy();
        }
        return;
    }

    if (!s_ntp_session || !s_wifi_events) {
        return;
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_ntp_retry < NTP_MAX_RETRY) {
            s_ntp_retry++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (!s_ntp_session || !s_wifi_events) {
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_ntp_retry = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t ensure_wifi_driver(void)
{
    if (s_driver_inited) {
        return ESP_OK;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    wifi_country_t country = {
        .cc = "01",
        .schan = 1,
        .nchan = 13,
        .policy = WIFI_COUNTRY_POLICY_MANUAL,
    };
    ESP_ERROR_CHECK(esp_wifi_set_country(&country));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    s_driver_inited = true;
    return ESP_OK;
}

static esp_err_t ensure_sta_netif(void)
{
    if (!s_sta_netif_created) {
        if (!esp_netif_create_default_wifi_sta()) {
            return ESP_FAIL;
        }
        s_sta_netif_created = true;
    }
    if (!s_ip_handler_registered) {
        ESP_ERROR_CHECK(
            esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL));
        s_ip_handler_registered = true;
    }
    if (!s_wifi_events) {
        s_wifi_events = xEventGroupCreate();
        if (!s_wifi_events) {
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static void radio_do_ntp_sync(void)
{
    if (s_mode == BADGE_RADIO_MODE_SNIFFER) {
        ESP_LOGI(TAG, "NTP sync skipped (sniffer active)");
        return;
    }

    ESP_LOGI(TAG, "NTP sync: connecting Wi-Fi…");
    esp_err_t err = ensure_wifi_driver();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi driver: %s", esp_err_to_name(err));
        return;
    }
    err = ensure_sta_netif();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sta netif: %s", esp_err_to_name(err));
        return;
    }

    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_ntp_retry = 0;
    s_ntp_session = true;

    err = badge_wifi_station_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "station start: %s", esp_err_to_name(err));
        s_ntp_session = false;
        return;
    }
    s_radio_started = true;

    EventBits_t bits =
        xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                            pdMS_TO_TICKS(NTP_CONNECT_TIMEOUT_MS));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "NTP sync: Wi-Fi connect failed/timeout");
        s_ntp_session = false;
        (void)badge_wifi_station_stop();
        s_radio_started = false;
        return;
    }

    ESP_LOGI(TAG, "NTP sync: got IP, fetching time…");
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sntp init: %s", esp_err_to_name(err));
    } else {
        err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(NTP_SYNC_TIMEOUT_MS));
        if (err == ESP_OK) {
            badge_time_sync_mark_ok();
            ESP_LOGI(TAG, "NTP sync OK");
        } else {
            ESP_LOGW(TAG, "NTP sync wait: %s", esp_err_to_name(err));
        }
        esp_netif_sntp_deinit();
    }

    s_ntp_session = false;
    (void)badge_wifi_station_stop();
    s_radio_started = false;
    ESP_LOGI(TAG, "NTP sync: radio off");
}

static void radio_task(void *arg)
{
    (void)arg;
    radio_cmd_t cmd;

    while (true) {
        if (xQueueReceive(s_cmd_queue, &cmd, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (cmd.type == RADIO_CMD_CYCLE_CHANNEL) {
            if (s_mode == BADGE_RADIO_MODE_SNIFFER && s_radio_started) {
                badge_its_g5_cycle_channel();
            }
            continue;
        }
        if (cmd.type == RADIO_CMD_NTP_SYNC) {
            radio_do_ntp_sync();
            continue;
        }
        esp_err_t err = badge_radio_set_mode(cmd.mode);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mode switch failed: %s", esp_err_to_name(err));
        }
    }
}

esp_err_t badge_radio_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(badge_name_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    s_cmd_queue = xQueueCreate(4, sizeof(radio_cmd_t));
    if (!s_cmd_queue) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(radio_task, "radio", 6144, NULL, 8, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* Defer esp_wifi_init until sniffer/NTP — early init panics on C5. */
    ESP_ERROR_CHECK(badge_host_link_init());
    badge_host_link_set_active(false);

    ESP_LOGI(TAG, "radio ready — default mode: badge");
    return badge_radio_request_mode(BADGE_RADIO_MODE_BADGE);
}

esp_err_t badge_radio_request_mode(badge_radio_mode_t mode)
{
    if (!s_cmd_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    radio_cmd_t cmd = {.type = RADIO_CMD_SET_MODE, .mode = mode};
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t badge_radio_request_cycle_channel(void)
{
    if (!s_cmd_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    radio_cmd_t cmd = {.type = RADIO_CMD_CYCLE_CHANNEL, .mode = BADGE_RADIO_MODE_SNIFFER};
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t badge_radio_request_ntp_sync(void)
{
    if (!s_cmd_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    radio_cmd_t cmd = {.type = RADIO_CMD_NTP_SYNC, .mode = BADGE_RADIO_MODE_BADGE};
    if (xQueueSend(s_cmd_queue, &cmd, pdMS_TO_TICKS(200)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void ensure_badge_mode_services(void)
{
    badge_host_link_set_active(false);
    badge_setup_start();
}

esp_err_t badge_radio_set_mode(badge_radio_mode_t mode)
{
    if (mode == s_mode && (mode != BADGE_RADIO_MODE_SNIFFER || s_radio_started)) {
        if (mode == BADGE_RADIO_MODE_BADGE) {
            ensure_badge_mode_services();
        }
        return ESP_OK;
    }

    badge_radio_mode_t prev_mode = s_mode;
    if (prev_mode == BADGE_RADIO_MODE_BADGE && mode != BADGE_RADIO_MODE_BADGE) {
        badge_setup_stop();
    }

    if (s_radio_started && s_driver_inited) {
        radio_stop_wifi();
    }

    s_mode = mode;

    esp_err_t err = ESP_OK;
    if (mode == BADGE_RADIO_MODE_SNIFFER) {
        ESP_ERROR_CHECK(ensure_wifi_driver());
        esp_log_level_set("its_g5", ESP_LOG_WARN);
        esp_log_level_set("wifi", ESP_LOG_WARN);
        badge_host_link_set_active(true);
        err = badge_its_g5_sniffer_start();
        if (err == ESP_OK) {
            s_radio_started = true;
        }
    } else {
        esp_log_level_set("its_g5", ESP_LOG_WARN);
        esp_log_level_set("wifi", ESP_LOG_WARN);
        badge_host_link_set_active(false);
        if (mode == BADGE_RADIO_MODE_BADGE) {
            ensure_badge_mode_services();
        }
        ESP_LOGI(TAG, "radio off — %s mode", mode == BADGE_RADIO_MODE_SCHEDULE ? "schedule" : "badge");
    }

    badge_host_link_send_mode(mode);

    static const char *labels[] = {
        [BADGE_RADIO_MODE_SNIFFER] = "802.11p sniffer",
        [BADGE_RADIO_MODE_SCHEDULE] = "schedule",
        [BADGE_RADIO_MODE_BADGE] = "badge",
    };
    ESP_LOGI(TAG, "mode → %s", labels[mode]);
    return err;
}

badge_radio_mode_t badge_radio_get_mode(void)
{
    return s_mode;
}

bool badge_radio_is_badge_mode(void)
{
    return s_mode == BADGE_RADIO_MODE_BADGE;
}

void badge_radio_refresh_power_profile(void)
{
}

uint32_t badge_radio_rx_count(void)
{
    return badge_its_g5_packet_count();
}

int badge_radio_its_channel_mhz(void)
{
    return badge_its_g5_get_channel_mhz();
}

const char *badge_radio_get_ip(void)
{
    return NULL;
}
