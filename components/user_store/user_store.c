#include "user_store.h"

#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "user_store";

static const esp_partition_t *s_part;
static user_store_header_t s_hdr;
static bool s_hdr_ok;

static uint32_t header_crc(const user_store_header_t *h)
{
    const uint8_t *p = (const uint8_t *)h;
    size_t n = offsetof(user_store_header_t, crc32);
    uint32_t s = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        s ^= p[i];
        s *= 16777619u;
    }
    return s;
}

static esp_err_t load_header(void)
{
    memset(&s_hdr, 0, sizeof(s_hdr));
    s_hdr_ok = false;
    if (!s_part) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_partition_read(s_part, 0, &s_hdr, sizeof(s_hdr));
    if (err != ESP_OK) {
        return err;
    }

    if (s_hdr.magic != USER_STORE_MAGIC || s_hdr.layout_ver != USER_STORE_LAYOUT_VER) {
        ESP_LOGW(TAG, "no valid user header (magic=0x%08lx)", (unsigned long)s_hdr.magic);
        return ESP_ERR_NOT_FOUND;
    }

    if (s_hdr.crc32 != 0 && s_hdr.crc32 != header_crc(&s_hdr)) {
        ESP_LOGW(TAG, "user header CRC mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    s_hdr.name[USER_STORE_NAME_MAX - 1] = '\0';
    s_hdr.wifi_ssid[USER_STORE_SSID_MAX - 1] = '\0';
    s_hdr.wifi_pass[USER_STORE_PASS_MAX - 1] = '\0';
    s_hdr_ok = true;
    return ESP_OK;
}

static esp_err_t save_header(void)
{
    if (!s_part) {
        return ESP_ERR_INVALID_STATE;
    }

    s_hdr.magic = USER_STORE_MAGIC;
    s_hdr.layout_ver = USER_STORE_LAYOUT_VER;
    s_hdr.crc32 = header_crc(&s_hdr);

    esp_err_t err = esp_partition_erase_range(s_part, 0, USER_STORE_HDR_SIZE);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_partition_write(s_part, 0, &s_hdr, sizeof(s_hdr));
    if (err == ESP_OK) {
        s_hdr_ok = true;
    }
    return err;
}

esp_err_t user_store_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "user");
    if (!s_part) {
        ESP_LOGE(TAG, "user partition not found");
        return ESP_ERR_NOT_FOUND;
    }
    if (s_part->size < USER_STORE_PART_SIZE) {
        ESP_LOGW(TAG, "user partition smaller than expected (%lu)", (unsigned long)s_part->size);
    }
    return load_header();
}

bool user_store_header_valid(void)
{
    return s_hdr_ok;
}

const user_store_header_t *user_store_header(void)
{
    return s_hdr_ok ? &s_hdr : NULL;
}

esp_err_t user_store_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    if (!s_hdr_ok || !ssid || ssid_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_hdr.wifi_ssid[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(ssid, s_hdr.wifi_ssid, ssid_len);
    if (pass && pass_len) {
        strlcpy(pass, s_hdr.wifi_pass, pass_len);
    }
    return ESP_OK;
}

esp_err_t user_store_set_wifi(const char *ssid, const char *pass)
{
    if (!ssid) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_hdr_ok) {
        memset(&s_hdr, 0, sizeof(s_hdr));
    }
    strlcpy(s_hdr.wifi_ssid, ssid, sizeof(s_hdr.wifi_ssid));
    strlcpy(s_hdr.wifi_pass, pass ? pass : "", sizeof(s_hdr.wifi_pass));
    return save_header();
}

esp_err_t user_store_get_name(char *name, size_t name_len)
{
    if (!s_hdr_ok || !name || name_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_hdr.name[0] == '\0') {
        return ESP_ERR_NOT_FOUND;
    }
    strlcpy(name, s_hdr.name, name_len);
    return ESP_OK;
}

esp_err_t user_store_set_name(const char *name)
{
    if (!name) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_hdr_ok) {
        memset(&s_hdr, 0, sizeof(s_hdr));
    }
    strlcpy(s_hdr.name, name, sizeof(s_hdr.name));
    return save_header();
}

bool user_store_bg_valid(void)
{
    return s_hdr_ok && (s_hdr.flags & USER_STORE_FLAG_BG_VALID);
}

esp_err_t user_store_read_bg(size_t offset, void *dst, size_t len)
{
    if (!s_part || !dst) {
        return ESP_ERR_INVALID_ARG;
    }
    if (offset + len > USER_STORE_BG_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    return esp_partition_read(s_part, USER_STORE_BG_OFFSET + offset, dst, len);
}

esp_err_t user_store_write_bg(const void *rgb565, size_t len)
{
    if (!s_part || !rgb565 || len != USER_STORE_BG_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Erase header+bg region covering both (aligned erase). */
    size_t erase_end = USER_STORE_BG_OFFSET + USER_STORE_BG_BYTES;
    size_t erase_size = (erase_end + 4095) & ~4095u;
    if (erase_size > s_part->size) {
        erase_size = s_part->size;
    }

    esp_err_t err = esp_partition_erase_range(s_part, 0, erase_size);
    if (err != ESP_OK) {
        return err;
    }

    if (!s_hdr_ok) {
        memset(&s_hdr, 0, sizeof(s_hdr));
    }
    s_hdr.magic = USER_STORE_MAGIC;
    s_hdr.layout_ver = USER_STORE_LAYOUT_VER;
    s_hdr.flags |= USER_STORE_FLAG_BG_VALID;
    s_hdr.crc32 = header_crc(&s_hdr);

    err = esp_partition_write(s_part, 0, &s_hdr, sizeof(s_hdr));
    if (err != ESP_OK) {
        return err;
    }
    err = esp_partition_write(s_part, USER_STORE_BG_OFFSET, rgb565, len);
    if (err == ESP_OK) {
        s_hdr_ok = true;
    }
    return err;
}
