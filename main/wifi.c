#include "wifi.h"

#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "user_store.h"
#include "wifi_credentials.h"

static const char *TAG = "wifi";

esp_err_t badge_wifi_station_start(void)
{
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    char ssid[USER_STORE_SSID_MAX];
    char pass[USER_STORE_PASS_MAX];
    const char *use_ssid = WIFI_SSID;
    const char *use_pass = WIFI_PASSWORD;

    if (user_store_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK) {
        use_ssid = ssid;
        use_pass = pass;
        ESP_LOGI(TAG, "using Wi-Fi creds from user partition");
    } else {
        ESP_LOGI(TAG, "using Wi-Fi creds from wifi.conf build fallback");
    }

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, use_ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, use_pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_err_t err = esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_band_mode: %s", esp_err_to_name(err));
    }

    /* esp_wifi_connect() is triggered from WIFI_EVENT_STA_START in radio.c */
    ESP_LOGI(TAG, "station starting, connecting to \"%s\"…", use_ssid);
    return ESP_OK;
}

esp_err_t badge_wifi_station_stop(void)
{
    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "station stopped");
    return ESP_OK;
}
