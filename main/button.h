#pragma once

#include "esp_err.h"

typedef enum {
    BADGE_BTN_SW_A = 0,
    BADGE_BTN_SW_B,
    BADGE_BTN_SW_C,
    BADGE_BTN_SW_D,
    BADGE_BTN_SW_BOOT,
    BADGE_BTN_COUNT,
} badge_btn_id_t;

typedef void (*badge_button_cb_t)(badge_btn_id_t btn, void *ctx);

esp_err_t badge_button_init(badge_button_cb_t cb, void *ctx);
