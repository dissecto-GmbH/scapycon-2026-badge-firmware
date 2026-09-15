#pragma once

#include "esp_err.h"

#define BADGE_NAME_MAX 24

esp_err_t badge_name_init(void);
const char *badge_name_get(void);
esp_err_t badge_name_set(const char *name);

typedef void (*badge_name_change_cb_t)(void);
void badge_name_set_change_cb(badge_name_change_cb_t cb);
