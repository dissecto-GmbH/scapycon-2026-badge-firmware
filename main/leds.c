#include "leds.h"

#include <string.h>

#include "board.h"
#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip_encoder.h"
#include "power.h"

static const char *TAG = "leds";

#define RMT_RESOLUTION_HZ 10000000

static rmt_channel_handle_t s_channel;
static rmt_encoder_handle_t s_encoder;
static uint8_t s_pixels[BADGE_LED_COUNT * 3];

esp_err_t badge_leds_init(void)
{
    rmt_tx_channel_config_t tx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = BADGE_LED_PIN,
        .mem_block_symbols = 64,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .trans_queue_depth = 4,
    };
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_cfg, &s_channel));

    led_strip_encoder_config_t enc_cfg = {
        .resolution = RMT_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_led_strip_encoder(&enc_cfg, &s_encoder));
    ESP_ERROR_CHECK(rmt_enable(s_channel));

    memset(s_pixels, 0, sizeof(s_pixels));
    ESP_LOGI(TAG, "%d SK6805 LEDs on GPIO%d", BADGE_LED_COUNT, BADGE_LED_PIN);
    return ESP_OK;
}

void badge_leds_set(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    if (index >= BADGE_LED_COUNT) {
        return;
    }
    s_pixels[index * 3 + 0] = g;
    s_pixels[index * 3 + 1] = r;
    s_pixels[index * 3 + 2] = b;
}

void badge_leds_refresh(void)
{
    uint8_t percent =
        badge_power_usb_present() ? BADGE_LED_USB_PERCENT : BADGE_LED_BATTERY_PERCENT;
    uint8_t scaled[BADGE_LED_COUNT * 3];
    if (percent >= 100) {
        memcpy(scaled, s_pixels, sizeof(scaled));
    } else {
        for (size_t i = 0; i < sizeof(scaled); i++) {
            scaled[i] = (uint8_t)((s_pixels[i] * (uint16_t)percent) / 100u);
        }
    }

    rmt_transmit_config_t tx_config = {
        .loop_count = 0,
    };

    esp_err_t err = rmt_encoder_reset(s_encoder);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "encoder reset: %s", esp_err_to_name(err));
        return;
    }
    err = rmt_transmit(s_channel, s_encoder, scaled, sizeof(scaled), &tx_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "transmit: %s", esp_err_to_name(err));
        return;
    }
    err = rmt_tx_wait_all_done(s_channel, portMAX_DELAY);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wait done: %s", esp_err_to_name(err));
    }
}
