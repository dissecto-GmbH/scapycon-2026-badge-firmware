#include "power.h"

#include "board.h"
#include "display.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "radio.h"
#include "soc/adc_channel.h"

static const char *TAG = "power";

/* GPIO2 = ADC1 channel 1 on ESP32-C5. */
#define VBUS_ADC_UNIT     ADC_UNIT_1
#define VBUS_ADC_CHANNEL  ADC1_GPIO2_CHANNEL

static bool s_usb_present;
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;

static void apply_backlight(bool usb_present)
{
    uint8_t percent = usb_present ? BADGE_BACKLIGHT_USB_PERCENT : BADGE_BACKLIGHT_BATTERY_PERCENT;
    badge_display_set_brightness(percent);
}

static bool read_vbus_mv(int *out_mv, int *out_raw)
{
    int raw = 0;
    esp_err_t err = adc_oneshot_read(s_adc, VBUS_ADC_CHANNEL, &raw);
    if (err != ESP_OK) {
        return false;
    }
    if (out_raw) {
        *out_raw = raw;
    }

    int mv = 0;
    if (s_cali_ok && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
        if (out_mv) {
            *out_mv = mv;
        }
        return true;
    }

    /* Fallback: 12-bit full-scale ≈ 3300 mV at ATTEN_DB_12. */
    mv = (raw * 3300) / 4095;
    if (out_mv) {
        *out_mv = mv;
    }
    return true;
}

esp_err_t badge_power_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = VBUS_ADC_UNIT,
    };
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

    int mv = 0;
    int raw = 0;
    if (read_vbus_mv(&mv, &raw)) {
        s_usb_present = mv >= BADGE_VBUS_ADC_PRESENT_MV;
        ESP_LOGI(TAG, "VBUS ADC GPIO%d raw=%d %dmV — %s (thr %dmV)", BADGE_VBUS_SNS_PIN, raw, mv,
                 s_usb_present ? "USB power" : "battery", BADGE_VBUS_ADC_PRESENT_MV);
    } else {
        s_usb_present = false;
        ESP_LOGW(TAG, "VBUS ADC read failed — assuming battery");
    }
    return ESP_OK;
}

bool badge_power_usb_present(void)
{
    int mv = 0;
    if (!s_adc || !read_vbus_mv(&mv, NULL)) {
        return s_usb_present;
    }
    return mv >= BADGE_VBUS_ADC_PRESENT_MV;
}

static void power_monitor_task(void *arg)
{
    (void)arg;

    s_usb_present = badge_power_usb_present();
    apply_backlight(s_usb_present);

    while (true) {
        bool usb = badge_power_usb_present();
        if (usb != s_usb_present) {
            s_usb_present = usb;
            ESP_LOGI(TAG, "power source → %s", usb ? "USB" : "battery");
            apply_backlight(usb);
            if (badge_radio_is_badge_mode()) {
                badge_radio_refresh_power_profile();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void badge_power_start_monitor(void)
{
    xTaskCreate(power_monitor_task, "power", 3072, NULL, 3, NULL);
}
