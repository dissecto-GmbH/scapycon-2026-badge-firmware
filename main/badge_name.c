#include "badge_name.h"

#include <ctype.h>
#include <string.h>

#include "badge_config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "user_store.h"
#include "utf8_name.h"

static const char *TAG = "badge_name";

static char s_name[BADGE_NAME_MAX + 1];
static badge_name_change_cb_t s_change_cb;

static void trim_inplace(char *s)
{
    if (!s || !*s) {
        return;
    }

    char *start = s;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }

    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) {
        s[--len] = '\0';
    }
}

static void sanitize_name(char *dst, size_t dst_len, const char *src)
{
    size_t n = 0;
    const char *p = src;
    while (p && *p && n + 1 < dst_len) {
        uint32_t cp;
        size_t nb;
        if (!utf8_next(p, &cp, &nb)) {
            p++;
            continue;
        }
        if (utf8_name_codepoint_allowed(cp) && n + nb < dst_len) {
            for (size_t i = 0; i < nb; i++) {
                dst[n++] = p[i];
            }
        }
        p += nb;
    }
    dst[n] = '\0';
    trim_inplace(dst);
}

esp_err_t badge_name_init(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open("badge", NVS_READONLY, &h);
    if (err == ESP_OK) {
        size_t len = sizeof(s_name);
        err = nvs_get_str(h, "name", s_name, &len);
        nvs_close(h);
        if (err == ESP_OK && s_name[0] != '\0') {
            ESP_LOGI(TAG, "loaded name from NVS: %s", s_name);
            return ESP_OK;
        }
    }

    char from_user[USER_STORE_NAME_MAX];
    if (user_store_get_name(from_user, sizeof(from_user)) == ESP_OK) {
        sanitize_name(s_name, sizeof(s_name), from_user);
        if (s_name[0] != '\0') {
            ESP_LOGI(TAG, "loaded name from user partition: %s", s_name);
            return ESP_OK;
        }
    }

    sanitize_name(s_name, sizeof(s_name), BADGE_DISPLAY_NAME);
    ESP_LOGI(TAG, "using build-time name: %s", s_name);
    return ESP_OK;
}

const char *badge_name_get(void)
{
    return s_name;
}

esp_err_t badge_name_set(const char *name)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }

    char cleaned[BADGE_NAME_MAX + 1];
    sanitize_name(cleaned, sizeof(cleaned), name);
    if (cleaned[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (strcmp(cleaned, s_name) == 0) {
        return ESP_OK;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open("badge", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(h, "name", cleaned);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs save: %s", esp_err_to_name(err));
        return err;
    }

    strlcpy(s_name, cleaned, sizeof(s_name));
    ESP_LOGI(TAG, "name saved: %s", s_name);
    if (s_change_cb) {
        s_change_cb();
    }
    return ESP_OK;
}

void badge_name_set_change_cb(badge_name_change_cb_t cb)
{
    s_change_cb = cb;
}
