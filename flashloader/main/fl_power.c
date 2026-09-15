#include "fl_power.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "soc/adc_channel.h"

static const char *TAG = "fl_pwr";

#define VBUS_ADC_UNIT    ADC_UNIT_1
#define VBUS_ADC_CHANNEL ADC1_GPIO2_CHANNEL

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;

esp_err_t fl_gpio_init_sw_d(void)
{
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BADGE_BTN_PIN_D,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&btn);
}

esp_err_t fl_power_adc_init(void)
{
    if (s_adc) {
        return ESP_OK;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = VBUS_ADC_UNIT };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc);
    if (err != ESP_OK) {
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    err = adc_oneshot_config_channel(s_adc, VBUS_ADC_CHANNEL, &chan_cfg);
    if (err != ESP_OK) {
        return err;
    }

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = VBUS_ADC_UNIT,
        .chan = VBUS_ADC_CHANNEL,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) == ESP_OK) {
        s_cali_ok = true;
    }
#endif
    return ESP_OK;
}

esp_err_t fl_power_init(void)
{
    esp_err_t err = fl_gpio_init_sw_d();
    if (err != ESP_OK) {
        return err;
    }
    return fl_power_adc_init();
}

bool fl_power_usb_present(void)
{
    if (!s_adc) {
        return false;
    }
    int raw = 0;
    if (adc_oneshot_read(s_adc, VBUS_ADC_CHANNEL, &raw) != ESP_OK) {
        return false;
    }
    int mv = 0;
    if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        ESP_LOGI(TAG, "VBUS %dmV", mv);
        return mv >= BADGE_VBUS_ADC_PRESENT_MV;
    }
    mv = (raw * 3300) / 4095;
    ESP_LOGI(TAG, "VBUS ~%dmV (uncal)", mv);
    return mv >= BADGE_VBUS_ADC_PRESENT_MV;
}

bool fl_sw_d_held(void)
{
    /* Active low */
    return gpio_get_level(BADGE_BTN_PIN_D) == 0;
}
