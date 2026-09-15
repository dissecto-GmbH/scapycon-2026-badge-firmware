#include "time_sync.h"

#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio.h"

static const char *TAG = "time_sync";

#define NTP_BOOT_DELAY_MS 3000

static volatile bool s_synced;

bool badge_time_is_synced(void)
{
    return s_synced;
}

void badge_time_sync_mark_ok(void)
{
    s_synced = true;
}

static void boot_sync_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(NTP_BOOT_DELAY_MS));
    esp_err_t err = badge_radio_request_ntp_sync();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NTP sync request failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}

esp_err_t badge_time_sync_start(void)
{
    /* Scapycon / Munich local time (CET/CEST). */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    BaseType_t ok = xTaskCreate(boot_sync_task, "ntp_boot", 2048, NULL, 5, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "NTP: boot sync queued (no periodic re-sync)");
    return ESP_OK;
}
