#include "board.h"
#include "button.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leds.h"
#include "power.h"
#include "radio.h"
#include "ui.h"
#include "user_store.h"

static const char *TAG = "demo";

/* Chain order on PCB: U6 → U7 → U9 → U10 → U11 → U12 */
#define LED_U6  0
#define LED_U7  1
#define LED_U9  2
#define LED_U10 3
#define LED_U11 4
#define LED_U12 5

#define PULSE_PERIOD_MS    2500
#define SPECTRUM_PERIOD_MS 8000

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint8_t pulse_brightness(uint32_t period_ms)
{
    uint32_t t = now_ms() % period_ms;
    uint32_t half = period_ms / 2;
    uint32_t level;

    if (t < half) {
        level = 40 + (215 * t) / half;
    } else {
        level = 40 + (215 * (period_ms - t)) / half;
    }
    return (uint8_t)level;
}

static void spectrum_rgb(uint8_t *r, uint8_t *g, uint8_t *b)
{
    /* 0..1535 triangle wave across R→G→B→R */
    uint32_t step = (now_ms() % SPECTRUM_PERIOD_MS) * 1536 / SPECTRUM_PERIOD_MS;
    uint32_t seg = step / 512;
    uint32_t frac = step % 512;

    switch (seg) {
    case 0:
        *r = (uint8_t)(255 - frac / 2);
        *g = (uint8_t)(frac / 2);
        *b = 0;
        break;
    case 1:
        *r = 0;
        *g = (uint8_t)(255 - frac / 2);
        *b = (uint8_t)(frac / 2);
        break;
    default:
        *r = (uint8_t)(frac / 2);
        *g = 0;
        *b = (uint8_t)(255 - frac / 2);
        break;
    }
}

static void led_task(void *arg)
{
    (void)arg;
    while (true) {
        uint8_t g = pulse_brightness(PULSE_PERIOD_MS);
        badge_leds_set(LED_U6, 0, g, 0);
        badge_leds_set(LED_U7, 0, g, 0);

        uint8_t b = pulse_brightness(PULSE_PERIOD_MS);
        badge_leds_set(LED_U11, 0, 0, b);
        badge_leds_set(LED_U12, 0, 0, b);

        uint8_t r9, g9, b9;
        spectrum_rgb(&r9, &g9, &b9);
        badge_leds_set(LED_U9, r9, g9, b9);
        badge_leds_set(LED_U10, r9, g9, b9);

        badge_leds_refresh();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void button_flash_feedback(void)
{
    for (int i = 0; i < BADGE_LED_COUNT; i++) {
        badge_leds_set(i, 48, 48, 48);
    }
    badge_leds_refresh();
    vTaskDelay(pdMS_TO_TICKS(80));
}

static badge_radio_mode_t mode_for_button(badge_btn_id_t btn)
{
    switch (btn) {
    case BADGE_BTN_SW_A:
        return BADGE_RADIO_MODE_BADGE;
    case BADGE_BTN_SW_B:
        return BADGE_RADIO_MODE_SNIFFER;
    case BADGE_BTN_SW_C:
        return BADGE_RADIO_MODE_SCHEDULE;
    default:
        return BADGE_RADIO_MODE_BADGE;
    }
}

static void on_button(badge_btn_id_t btn, void *ctx)
{
    (void)ctx;

    if (btn > BADGE_BTN_SW_D) {
        return;
    }

    button_flash_feedback();

    if (btn == BADGE_BTN_SW_D) {
        ESP_LOGI(TAG, "SW_D: reboot into flashloader (hold at reset + USB for OTA)");
        const esp_partition_t *factory =
            esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
        if (factory) {
            esp_ota_set_boot_partition(factory);
            esp_restart();
        }
        return;
    }

    badge_radio_mode_t mode = mode_for_button(btn);
    if (mode == badge_radio_get_mode()) {
        if (btn == BADGE_BTN_SW_B && mode == BADGE_RADIO_MODE_SNIFFER) {
            esp_err_t err = badge_radio_request_cycle_channel();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "channel cycle failed: %s", esp_err_to_name(err));
            }
        } else if (btn == BADGE_BTN_SW_C && mode == BADGE_RADIO_MODE_SCHEDULE) {
            badge_ui_toggle_schedule_day();
        }
        return;
    }

    esp_err_t err = badge_radio_request_mode(mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mode request failed: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Scapycon 2026 — V2X badge");

    /* Next reset returns to factory flashloader so SW_D+USB OTA stays reachable. */
    const esp_partition_t *factory =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    if (factory) {
        esp_err_t err = esp_ota_set_boot_partition(factory);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "set boot factory: %s", esp_err_to_name(err));
        }
    }

    (void)user_store_init();

    ESP_ERROR_CHECK(badge_power_init());
    ESP_ERROR_CHECK(badge_leds_init());
    ESP_ERROR_CHECK(badge_radio_init());
    ESP_ERROR_CHECK(badge_button_init(on_button, NULL));

    xTaskCreate(led_task, "leds", 4096, NULL, 5, NULL);
    badge_ui_start();
}
