#include "radio.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "badge_name.h"
#include "badge_setup.h"
#include "host_link.h"
#include "its_g5.h"
#include "nvs_flash.h"

static const char *TAG = "radio";

typedef enum {
    RADIO_CMD_SET_MODE = 0,
    RADIO_CMD_CYCLE_CHANNEL,
} radio_cmd_type_t;

typedef struct {
    radio_cmd_type_t type;
    badge_radio_mode_t mode;
} radio_cmd_t;

static badge_radio_mode_t s_mode = BADGE_RADIO_MODE_BADGE;
static QueueHandle_t s_cmd_queue;
static bool s_driver_inited;
static bool s_radio_started;

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
        esp_err_t err = badge_radio_set_mode(cmd.mode);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "mode switch failed: %s", esp_err_to_name(err));
        }
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;

    if (base == WIFI_EVENT && id == WIFI_EVENT_WIFI_READY) {
        if (s_mode == BADGE_RADIO_MODE_SNIFFER) {
            badge_its_g5_apply_phy();
        }
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

    BaseType_t ok = xTaskCreate(radio_task, "radio", 4096, NULL, 8, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* Defer esp_wifi_init until sniffer mode — early init panics on C5. */
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
        if (s_mode == BADGE_RADIO_MODE_SNIFFER) {
            badge_its_g5_sniffer_stop();
        }
        esp_err_t stop_err = esp_wifi_stop();
        if (stop_err != ESP_OK && stop_err != ESP_ERR_WIFI_NOT_STARTED) {
            ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(stop_err));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        s_radio_started = false;
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
