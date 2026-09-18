#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "badge_name.h"
#include "board.h"
#include "display.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "font8x8_basic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "its_g5.h"
#include "power.h"
#include "radio.h"
#include "time_sync.h"
#include "utf8_name.h"

static const char *TAG = "ui";

#define COLOR_NAVY   0x0010
#define COLOR_GREEN  0x07E0
#define COLOR_YELLOW 0xFFE0
#define COLOR_ORANGE 0xFD20
#define COLOR_WHITE  0xFFFF
#define COLOR_BLACK  0x0000
/* Logo green ≈ #2BAA6F */
#define COLOR_BRAND  0x2D4D

#define TEXT_X       8
#define TEXT_W       (BADGE_LCD_H_RES - TEXT_X * 2)
#define TEXT_SCALE   2
#define LINE_H       22
#define CHAR_H       (8 * TEXT_SCALE)
#define SCHED_SCALE  1
#define SCHED_LINE_H 9

#define Y_TITLE      12
#define Y_MODE       (Y_TITLE + CHAR_H + 6)
#define Y_LINE2      (Y_MODE + LINE_H)
#define Y_LINE3      (Y_LINE2 + LINE_H)
#define Y_PKT        Y_MODE
#define Y_CH         Y_LINE2
#define Y_HEX        Y_LINE3
#define HEX_SCALE    1
#define HEX_LINE_H   9
#define HEX_BPL      16

static esp_lcd_panel_handle_t s_panel;

typedef struct {
    badge_radio_mode_t mode;
    uint32_t rx_count;
    int channel_mhz;
} ui_state_t;

static ui_state_t s_ui;
static bool s_ui_ready;
static volatile bool s_name_dirty;
static volatile bool s_sched_dirty;
static int s_sched_day; /* 0 = day 1, 1 = day 2 */
static int s_drawn_minute = -1; /* hour*60+min of last painted clock; -1 = none */

#define CLOCK_SCALE 2
#define CLOCK_MARGIN 8

/* 1-bit sniffer framebuffer: 320×240 / 8 ≈ 9.4 KiB — compose, then blit dirty rows only. */
#define MONO_STRIDE ((BADGE_LCD_H_RES + 7) / 8)
static uint8_t s_mono_fb[MONO_STRIDE * BADGE_LCD_V_RES];
static uint8_t s_mono_shown[MONO_STRIDE * BADGE_LCD_V_RES];
static bool s_sniffer_on_screen;

static void mono_clear(void)
{
    memset(s_mono_fb, 0, sizeof(s_mono_fb));
}

static void mono_set(int x, int y)
{
    if ((unsigned)x >= (unsigned)BADGE_LCD_H_RES || (unsigned)y >= (unsigned)BADGE_LCD_V_RES) {
        return;
    }
    s_mono_fb[(size_t)y * MONO_STRIDE + (size_t)(x >> 3)] |= (uint8_t)(1u << (x & 7));
}

static void mono_draw_glyph(int x, int y, unsigned idx, int scale)
{
    if (idx >= UTF8_GLYPH_COUNT) {
        idx = (unsigned)'?';
    }
    const uint8_t *glyph = font8x8_basic[idx];
    for (int row = 0; row < 8; row++) {
        for (int col = 0; col < 8; col++) {
            if (!(glyph[row] & (1 << col))) {
                continue;
            }
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    mono_set(x + col * scale + sx, y + row * scale + sy);
                }
            }
        }
    }
}

static void mono_draw_string(int x, int y, const char *str, int scale)
{
    if (!str || scale < 1) {
        return;
    }
    int cx = x;
    while (*str) {
        uint32_t cp;
        size_t nb;
        if (!utf8_next(str, &cp, &nb)) {
            str++;
            continue;
        }
        mono_draw_glyph(cx, y, utf8_codepoint_to_glyph(cp), scale);
        cx += 8 * scale + 1;
        str += nb;
    }
}

static const badge_mono_color_band_t s_sniffer_bands[] = {
    {Y_PKT, Y_PKT + CHAR_H, COLOR_GREEN, COLOR_NAVY},
    {Y_CH, Y_CH + CHAR_H, COLOR_YELLOW, COLOR_NAVY},
};

static void sniffer_flush_dirty(void)
{
    const size_t nbands = sizeof(s_sniffer_bands) / sizeof(s_sniffer_bands[0]);

    if (!s_sniffer_on_screen) {
        badge_display_blit_mono(s_panel, s_mono_fb, MONO_STRIDE, 0, BADGE_LCD_V_RES, s_sniffer_bands,
                                nbands, COLOR_WHITE, COLOR_NAVY);
        memcpy(s_mono_shown, s_mono_fb, sizeof(s_mono_shown));
        s_sniffer_on_screen = true;
        return;
    }

    int run0 = -1;
    for (int y = 0; y <= BADGE_LCD_V_RES; y++) {
        bool dirty = false;
        if (y < BADGE_LCD_V_RES) {
            dirty = memcmp(&s_mono_fb[(size_t)y * MONO_STRIDE],
                           &s_mono_shown[(size_t)y * MONO_STRIDE], MONO_STRIDE) != 0;
        }
        if (dirty) {
            if (run0 < 0) {
                run0 = y;
            }
        } else if (run0 >= 0) {
            badge_display_blit_mono(s_panel, s_mono_fb, MONO_STRIDE, run0, y, s_sniffer_bands, nbands,
                                    COLOR_WHITE, COLOR_NAVY);
            memcpy(&s_mono_shown[(size_t)run0 * MONO_STRIDE], &s_mono_fb[(size_t)run0 * MONO_STRIDE],
                   (size_t)(y - run0) * MONO_STRIDE);
            run0 = -1;
        }
    }
}

static void draw_sniffer_screen(void)
{
    mono_clear();

    mono_draw_string(TEXT_X, Y_TITLE, "V2X 802.11p Sniffer", TEXT_SCALE);

    char line[40];
    snprintf(line, sizeof(line), "Packets: %lu", (unsigned long)badge_radio_rx_count());
    mono_draw_string(TEXT_X, Y_PKT, line, TEXT_SCALE);

    int mhz = badge_radio_its_channel_mhz();
    snprintf(line, sizeof(line), "Ch: %d MHz %s", mhz, mhz == 5900 ? "G5CC" : "G5SC");
    mono_draw_string(TEXT_X, Y_CH, line, TEXT_SCALE);

    uint8_t pkt[512];
    uint16_t len = badge_its_g5_copy_last_packet(pkt, sizeof(pkt));
    int y = Y_HEX;
    for (uint16_t off = 0; off < len && y + 8 <= BADGE_LCD_V_RES; off += HEX_BPL) {
        uint16_t n = (uint16_t)(len - off);
        if (n > HEX_BPL) {
            n = HEX_BPL;
        }
        for (uint16_t i = 0; i < n; i++) {
            snprintf(&line[i * 2], 3, "%02X", pkt[off + i]);
        }
        line[n * 2] = '\0';
        mono_draw_string(TEXT_X, y, line, HEX_SCALE);
        y += HEX_LINE_H;
    }

    sniffer_flush_dirty();
}

static const char *const s_day1[] = {
    "ScapyCon 2026 — Day 1",
    "SW_C toggles Day 1 / Day 2",
    "",
    "0800-0900 Registration",
    "0900-0915 Welcome (Weiss)",
    "0915-1000 Scapy Keynote (Rey)",
    "1000-1030 Connected cars (Pozzobon)",
    "1030-1100 Coffee Break",
    "1100-1145 V2X Wardriving",
    "          (Schuster & Puch)",
    "1145-1230 Garage to CI (Kugler)",
    "1230-1300 Beyond blind fuzzing",
    "          (Alkhatib)",
    "1300-1400 Lunch Break",
    "1400-1545 Workshop Melching/Horreis",
    "1545-1600 Coffee break",
    "1600-1800 Workshop Melching/Horreis",
    "1900-2300 Social Event",
};

static const char *const s_day2[] = {
    "ScapyCon 2026 — Day 2",
    "SW_C toggles Day 1 / Day 2",
    "",
    "0900-0945 Hidden BT (Vasquez Blanco)",
    "0945-1030 EU Cyber Resilience Act",
    "          (Funke & Krishnamurthy)",
    "1030-1100 Coffee break",
    "1100-1115 Trail of Bits (Thomas)",
    "1115-1130 Scapy Maintainers (Weiss)",
    "1130-1215 WHAD demo",
    "          (Cauquil & Cayre)",
    "1215-1255 Ethernet to CAN (Wiemer)",
    "1255-1300 Closing (Meisel)",
    "1300-1345 Lunch Break",
    "1345-1545 Workshops",
    "          Gardiner/Cauquil/Cayre/",
    "          Bagh/Kopf",
    "1545-1600 Coffee break",
    "1600-1800 Workshops (continued)",
};

static int string_pixel_width(const char *str, int scale)
{
    if (!str || !*str) {
        return 0;
    }
    int n = (int)utf8_char_count(str);
    if (n <= 0) {
        return 0;
    }
    return n * (8 * scale + 1) - 1;
}

static void draw_centered_name(int y, const char *name, int scale, uint16_t fg)
{
    int w = string_pixel_width(name, scale);
    int x = (BADGE_LCD_H_RES - w) / 2;
    if (x < TEXT_X) {
        x = TEXT_X;
    }
    badge_display_draw_string_transparent(s_panel, x, y, name, fg, COLOR_NAVY, scale);
}

static void draw_schedule(void)
{
    const char *const *lines = (s_sched_day == 0) ? s_day1 : s_day2;
    size_t nlines = (s_sched_day == 0) ? (sizeof(s_day1) / sizeof(s_day1[0]))
                                       : (sizeof(s_day2) / sizeof(s_day2[0]));

    badge_display_fill(s_panel, 0, 0, BADGE_LCD_H_RES, BADGE_LCD_V_RES, COLOR_BRAND);

    int y = 4;
    for (size_t i = 0; i < nlines; i++) {
        if (y + 8 > BADGE_LCD_V_RES) {
            break;
        }
        uint16_t fg = COLOR_WHITE;
        if (i == 0) {
            fg = COLOR_BLACK;
        } else if (i == 1) {
            fg = COLOR_YELLOW;
        }
        badge_display_draw_string(s_panel, 4, y, lines[i], fg, COLOR_BRAND, SCHED_SCALE);
        y += SCHED_LINE_H;
    }
}

static int badge_name_scale_for_len(size_t len)
{
    if (len <= 10) {
        return 3;
    }
    return 2;
}

static int split_name_words(const char *name, char words[][BADGE_NAME_MAX + 1], int max_words)
{
    int nwords = 0;
    const char *p = name ? name : "";

    while (*p && nwords < max_words) {
        while (*p == ' ') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *start = p;
        while (*p && *p != ' ') {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len >= sizeof(words[0])) {
            len = sizeof(words[0]) - 1;
        }
        memcpy(words[nwords], start, len);
        words[nwords][len] = '\0';
        nwords++;
    }

    if (nwords == 0 && name && name[0]) {
        strlcpy(words[0], name, sizeof(words[0]));
        nwords = 1;
    }
    return nwords;
}

static int current_minute_of_day(void)
{
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm) == NULL) {
        return -1;
    }
    return tm.tm_hour * 60 + tm.tm_min;
}

static void draw_clock(void)
{
    if (!badge_time_is_synced()) {
        s_drawn_minute = -1;
        return;
    }

    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm) == NULL) {
        return;
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);

    int w = string_pixel_width(buf, CLOCK_SCALE);
    int h = 8 * CLOCK_SCALE;
    int x = BADGE_LCD_H_RES - CLOCK_MARGIN - w;
    int y = BADGE_LCD_V_RES - CLOCK_MARGIN - h;
    if (x < 0) {
        x = 0;
    }
    if (y < 0) {
        y = 0;
    }

    badge_display_restore_bg(s_panel, x, y, w, h, COLOR_NAVY);
    badge_display_draw_string_transparent(s_panel, x, y, buf, COLOR_WHITE, COLOR_NAVY, CLOCK_SCALE);
    s_drawn_minute = tm.tm_hour * 60 + tm.tm_min;
}

static void draw_badge_lines(void)
{
    const char *name = badge_name_get();
    char words[4][BADGE_NAME_MAX + 1];
    int nwords = split_name_words(name, words, 4);
    if (nwords <= 0) {
        badge_display_restore_bg(s_panel, TEXT_X, Y_TITLE, TEXT_W, BADGE_LCD_V_RES - Y_TITLE,
                                 COLOR_NAVY);
        draw_clock();
        return;
    }

    size_t max_len = 0;
    for (int i = 0; i < nwords; i++) {
        size_t len = utf8_char_count(words[i]);
        if (len > max_len) {
            max_len = len;
        }
    }
    int scale = badge_name_scale_for_len(max_len);
    int char_h = 8 * scale;
    int gap = (scale >= 3) ? 6 : 4;
    int block_h = nwords * char_h + (nwords - 1) * gap;
    int y0 = (BADGE_LCD_V_RES - block_h) / 2;
    if (y0 < 8) {
        y0 = 8;
    }

    badge_display_restore_bg(s_panel, 0, 0, BADGE_LCD_H_RES, BADGE_LCD_V_RES, COLOR_NAVY);
    for (int i = 0; i < nwords; i++) {
        draw_centered_name(y0 + i * (char_h + gap), words[i], scale, COLOR_BLACK);
    }
    draw_clock();
}

static void redraw_mode_content(void)
{
    badge_radio_mode_t mode = badge_radio_get_mode();
    if (mode != BADGE_RADIO_MODE_SNIFFER) {
        s_sniffer_on_screen = false;
    }
    if (mode == BADGE_RADIO_MODE_BADGE) {
        draw_badge_lines();
        return;
    }
    if (mode == BADGE_RADIO_MODE_SCHEDULE) {
        draw_schedule();
        return;
    }

    draw_sniffer_screen();
}

static void ui_capture_state(void)
{
    s_ui.mode = badge_radio_get_mode();
    s_ui.rx_count = badge_radio_rx_count();
    s_ui.channel_mhz = badge_radio_its_channel_mhz();
}

static void ui_init_panel(void)
{
    redraw_mode_content();
    ui_capture_state();
    s_ui_ready = true;
}

static void ui_update(void)
{
    if (!s_ui_ready) {
        ui_init_panel();
        return;
    }

    badge_radio_mode_t mode = badge_radio_get_mode();
    if (mode != s_ui.mode) {
        redraw_mode_content();
        ui_capture_state();
        s_sched_dirty = false;
        return;
    }

    if (mode == BADGE_RADIO_MODE_BADGE) {
        if (s_name_dirty) {
            draw_badge_lines();
            s_name_dirty = false;
        } else {
            int minute = current_minute_of_day();
            if (badge_time_is_synced() && minute >= 0 && minute != s_drawn_minute) {
                draw_clock();
            }
        }
        return;
    }

    if (mode == BADGE_RADIO_MODE_SCHEDULE) {
        if (s_sched_dirty) {
            draw_schedule();
            s_sched_dirty = false;
        }
        return;
    }

    if (mode == BADGE_RADIO_MODE_SNIFFER) {
        uint32_t rx = badge_radio_rx_count();
        int ch = badge_radio_its_channel_mhz();
        if (rx != s_ui.rx_count || ch != s_ui.channel_mhz) {
            draw_sniffer_screen();
            s_ui.rx_count = rx;
            s_ui.channel_mhz = ch;
        }
    }
}

static void log_status(void)
{
    if (badge_radio_get_mode() == BADGE_RADIO_MODE_BADGE) {
        ESP_LOGI(TAG, "Badge mode — %s", badge_name_get());
    } else if (badge_radio_get_mode() == BADGE_RADIO_MODE_SCHEDULE) {
        ESP_LOGI(TAG, "Schedule — day %d", s_sched_day + 1);
    } else if (badge_radio_get_mode() == BADGE_RADIO_MODE_SNIFFER) {
        ESP_LOGI(TAG, "802.11p sniffer — packets: %lu", (unsigned long)badge_radio_rx_count());
    }
}

static void ui_task(void *arg)
{
    (void)arg;

    badge_display_init(&s_panel);

    if (badge_display_is_ready()) {
        badge_power_start_monitor();
        ui_init_panel();

        while (true) {
            ui_update();
            TickType_t delay = badge_radio_get_mode() == BADGE_RADIO_MODE_BADGE ? pdMS_TO_TICKS(500)
                                                                                : pdMS_TO_TICKS(250);
            vTaskDelay(delay);
        }
    }

    ESP_LOGI(TAG, "headless UI (no display)");
    while (true) {
        log_status();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void on_name_changed(void)
{
    s_name_dirty = true;
}

void badge_ui_toggle_schedule_day(void)
{
    s_sched_day ^= 1;
    s_sched_dirty = true;
}

void badge_ui_start(void)
{
    badge_name_set_change_cb(on_name_changed);
    xTaskCreate(ui_task, "ui", 4096, NULL, 4, NULL);
}
