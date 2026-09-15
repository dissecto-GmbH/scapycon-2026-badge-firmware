#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "fl_display.h"
#include "fl_power.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include "user_store.h"

static const char *TAG = "flashloader";

#define FW_VER_URL "https://munich.dissec.to/scapycon2026/firmware.ver"
#define FW_BIN_URL "https://munich.dissec.to/scapycon2026/firmware.bin"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_CONNECT_TIMEOUT_MS 30000

static EventGroupHandle_t s_wifi_events;
static esp_lcd_panel_handle_t s_panel;
static int s_retry;

static void boot_log(const char *msg)
{
    fl_display_log(s_panel, msg);
}

static void boot_main_app(void)
{
    const esp_partition_t *main_app =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!main_app) {
        boot_log("ERR: no ota_0");
        ESP_LOGE(TAG, "ota_0 partition missing");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }

    ESP_LOGI(TAG, "Booting main app (ota_0 @ 0x%lx)", (unsigned long)main_app->address);
    boot_log("Booting main app...");
    esp_err_t err = esp_ota_set_boot_partition(main_app);
    if (err != ESP_OK) {
        boot_log("ERR: set boot");
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
    esp_restart();
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < 8) {
            s_retry++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_retry = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t wifi_connect_from_user(void)
{
    char ssid[USER_STORE_SSID_MAX];
    char pass[USER_STORE_PASS_MAX];
    esp_err_t err = user_store_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
    if (err != ESP_OK) {
        boot_log("No Wi-Fi in user");
        return err;
    }

    char line[48];
    snprintf(line, sizeof(line), "Wi-Fi: %.20s", ssid);
    boot_log(line);

    s_wifi_events = xEventGroupCreate();
    s_retry = 0;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE,
                                           pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_CONNECTED_BIT) {
        boot_log("Wi-Fi OK");
        return ESP_OK;
    }
    boot_log("Wi-Fi failed");
    return ESP_FAIL;
}

static esp_err_t http_get_text(const char *url, char *out, size_t out_len)
{
    if (!out || out_len < 2) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    int n = esp_http_client_read(client, out, (int)out_len - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status != 200 || n < 0) {
        return ESP_FAIL;
    }
    out[n] = '\0';
    /* Trim trailing whitespace / newlines */
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' ')) {
        out[--n] = '\0';
    }
    return ESP_OK;
}

static void get_local_ota0_version(char *out, size_t out_len)
{
    out[0] = '\0';
    const esp_partition_t *main_app =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!main_app) {
        return;
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(main_app, &desc) == ESP_OK) {
        strlcpy(out, desc.version, out_len);
    }
}

static esp_err_t do_https_ota(void)
{
    boot_log("Downloading...");
    esp_http_client_config_t http = {
        .url = FW_BIN_URL,
        .timeout_ms = 60000,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota = {
        .http_config = &http,
    };
    esp_err_t err = esp_https_ota(&ota);
    if (err == ESP_OK) {
        boot_log("OTA OK");
    } else {
        char line[40];
        snprintf(line, sizeof(line), "OTA fail %s", esp_err_to_name(err));
        boot_log(line);
    }
    return err;
}

static void run_update_flow(void)
{
    boot_log("Update mode");
    if (user_store_init() != ESP_OK && !user_store_header_valid()) {
        /* init returns NOT_FOUND when empty — still OK if we can write later */
        boot_log("user store empty");
    }

    if (wifi_connect_from_user() != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        boot_main_app();
        return;
    }

    boot_log("Check version...");
    char remote[64];
    if (http_get_text(FW_VER_URL, remote, sizeof(remote)) != ESP_OK || remote[0] == '\0') {
        boot_log("ver fetch fail");
        vTaskDelay(pdMS_TO_TICKS(2000));
        boot_main_app();
        return;
    }

    char local[64];
    get_local_ota0_version(local, sizeof(local));
    char line[48];
    snprintf(line, sizeof(line), "loc:%.16s", local[0] ? local : "(none)");
    boot_log(line);
    snprintf(line, sizeof(line), "rem:%.16s", remote);
    boot_log(line);

    if (local[0] != '\0' && strcmp(local, remote) == 0) {
        boot_log("Up to date");
        vTaskDelay(pdMS_TO_TICKS(1000));
        boot_main_app();
        return;
    }

    if (do_https_ota() == ESP_OK) {
        boot_log("Reboot to app");
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
    boot_main_app();
}

void app_main(void)
{
    /* SW_D first — no NVS/LCD. Main app always re-selects factory as next boot,
     * so a hang here bricks the badge on every reset. */
    if (fl_gpio_init_sw_d() != ESP_OK) {
        ESP_LOGW(TAG, "SW_D GPIO init failed — booting main anyway");
        boot_main_app();
    }
    if (!fl_sw_d_held()) {
        ESP_LOGI(TAG, "SW_D open — boot main app");
        boot_main_app();
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    ESP_LOGI(TAG, "SW_D held — flashloader update path");
    fl_power_adc_init();
    if (fl_display_init(&s_panel) == ESP_OK) {
        fl_display_clear(s_panel, 0x0000);
        boot_log("Scapycon flashloader");
    } else {
        ESP_LOGW(TAG, "LCD init failed — headless");
        s_panel = NULL;
    }

    bool usb = fl_power_usb_present();
    boot_log("SW_D: held");
    boot_log(usb ? "USB: yes" : "USB: no");

    if (usb) {
        run_update_flow();
    } else {
        boot_log("Need USB for OTA");
        vTaskDelay(pdMS_TO_TICKS(1500));
        boot_main_app();
    }
}
