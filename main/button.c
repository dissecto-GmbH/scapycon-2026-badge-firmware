#include "button.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

#define DEBOUNCE_MS 50

static const int s_btn_pins[BADGE_BTN_COUNT] = {
    [BADGE_BTN_SW_A] = BADGE_BTN_PIN_A,
    [BADGE_BTN_SW_B] = BADGE_BTN_PIN_B,
    [BADGE_BTN_SW_C] = BADGE_BTN_PIN_C,
    [BADGE_BTN_SW_D] = BADGE_BTN_PIN_D,
    [BADGE_BTN_SW_BOOT] = BADGE_BTN_PIN_BOOT,
};

static const char *s_btn_names[BADGE_BTN_COUNT] = {
    [BADGE_BTN_SW_A] = "SW_A",
    [BADGE_BTN_SW_B] = "SW_B",
    [BADGE_BTN_SW_C] = "SW_C",
    [BADGE_BTN_SW_D] = "SW_D",
    [BADGE_BTN_SW_BOOT] = "SW_BOOT",
};

static badge_button_cb_t s_cb;
static void *s_ctx;

static void button_task(void *arg)
{
    (void)arg;
    bool last_stable[BADGE_BTN_COUNT];
    int stable_ms[BADGE_BTN_COUNT];

    for (int i = 0; i < BADGE_BTN_COUNT; i++) {
        last_stable[i] = true;
        stable_ms[i] = 0;
    }

    while (true) {
        for (int i = 0; i < BADGE_BTN_COUNT; i++) {
            bool level = gpio_get_level(s_btn_pins[i]);
            if (level == last_stable[i]) {
                stable_ms[i] = 0;
                continue;
            }
            stable_ms[i] += 20;
            if (stable_ms[i] >= DEBOUNCE_MS) {
                last_stable[i] = level;
                stable_ms[i] = 0;
                if (!last_stable[i] && s_cb) {
                    s_cb((badge_btn_id_t)i, s_ctx);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t badge_button_init(badge_button_cb_t cb, void *ctx)
{
    s_cb = cb;
    s_ctx = ctx;

    uint64_t pin_mask = 0;
    for (int i = 0; i < BADGE_BTN_COUNT; i++) {
        pin_mask |= 1ULL << s_btn_pins[i];
    }

    gpio_config_t cfg = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    BaseType_t ok = xTaskCreate(button_task, "button", 3072, NULL, 7, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "buttons (active low, internal pull-up):");
    for (int i = 0; i < BADGE_BTN_COUNT; i++) {
        ESP_LOGI(TAG, "  %s GPIO%d", s_btn_names[i], s_btn_pins[i]);
    }
    return ESP_OK;
}
