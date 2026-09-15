#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define USER_STORE_MAGIC      0x42444755u /* 'BDGU' */
#define USER_STORE_LAYOUT_VER 1
#define USER_STORE_NAME_MAX   24
#define USER_STORE_SSID_MAX   32
#define USER_STORE_PASS_MAX   64
#define USER_STORE_HDR_SIZE   0x1000
#define USER_STORE_BG_OFFSET  0x1000
#define USER_STORE_BG_BYTES   (320 * 240 * 2) /* RGB565 */
#define USER_STORE_PART_SIZE  0xC0000         /* 768 KiB */

#define USER_STORE_FLAG_BG_VALID  (1u << 0)

typedef struct {
    uint32_t magic;
    uint16_t layout_ver;
    uint16_t flags;
    char name[USER_STORE_NAME_MAX];
    char wifi_ssid[USER_STORE_SSID_MAX];
    char wifi_pass[USER_STORE_PASS_MAX];
    uint32_t crc32; /* of header fields before crc; 0 = unused */
} user_store_header_t;

_Static_assert(sizeof(user_store_header_t) <= USER_STORE_HDR_SIZE, "user header too large");

esp_err_t user_store_init(void);
bool user_store_header_valid(void);
const user_store_header_t *user_store_header(void);

esp_err_t user_store_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
esp_err_t user_store_set_wifi(const char *ssid, const char *pass);

esp_err_t user_store_get_name(char *name, size_t name_len);
esp_err_t user_store_set_name(const char *name);

bool user_store_bg_valid(void);
esp_err_t user_store_read_bg(size_t offset, void *dst, size_t len);

/** Write full RGB565 framebuffer (USER_STORE_BG_BYTES) and set BG_VALID. */
esp_err_t user_store_write_bg(const void *rgb565, size_t len);
