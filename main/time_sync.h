#pragma once

#include <stdbool.h>

#include "esp_err.h"

/** Queue a one-shot boot NTP sync. Safe after badge_radio_init(). */
esp_err_t badge_time_sync_start(void);

/** True after at least one successful NTP sync this boot. */
bool badge_time_is_synced(void);

/** Called by the radio NTP session after a successful sync. */
void badge_time_sync_mark_ok(void);
