#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// SCHED_DAYS_*, sched_other_days_t and the window decision itself.
#include "sched_window.h"

// What happens outside the on-window. Stored in NVS as a plain u8; keep the
// values stable.
typedef enum {
    SCHED_OFF_ACT_OFF = 0,   // blank the panel (brightness 0) - the default
    SCHED_OFF_ACT_DIM = 1,   // keep showing, at off_brightness
} sched_off_action_t;

#define SCHED_OFF_BRI_DEFAULT  8

// IANA zone name, e.g. "Europe/Zurich". Longest in the database is
// "America/Argentina/Buenos_Aires" at 30; 48 also leaves room for a raw
// POSIX rule, which is still accepted (tz.h).
#define SCHED_TZ_MAX  48

typedef struct {
    bool    enabled;
    uint8_t on_hour;
    uint8_t on_min;
    uint8_t off_hour;
    uint8_t off_min;
    uint8_t days;           // bitmask: bit0=Mon .. bit6=Sun
    // What a day outside `days` does: hold the off-action all day
    // (SCHED_OTHER_OFF, the default and the only pre-2.8.0 behaviour) or let
    // the panel run as if there were no schedule (SCHED_OTHER_RUN).
    uint8_t other_days;     // sched_other_days_t
    int16_t utc_offset;     // minutes from UTC (e.g. +60 for CET)
    uint8_t off_action;     // sched_off_action_t
    uint8_t off_brightness; // 1..255, applied when off_action = DIM
    // Timezone override. Empty = the fixed utc_offset above (historical
    // behaviour, refreshed from the browser on every save). When set, the
    // schedule follows this zone's rule instead - DST transitions included.
    char    tz[SCHED_TZ_MAX + 1];
} schedule_t;

esp_err_t scheduler_init(void);

void scheduler_get(schedule_t *out);
esp_err_t scheduler_set(const schedule_t *in);

// True while the schedule's off-window holds the brightness down (0 with
// action "off", off_brightness with "dim"). Callers use it to avoid pushing
// the user's brightness onto the panel behind the schedule's back.
bool scheduler_display_is_off(void);

// Re-run the brightness arbiter and push the result at the panel. Anything
// that changes an input the arbiter reads - the stored brightness, the light
// sensor's level - calls this instead of touching led_driver itself, so the
// off-window, the sensor and the fallback dim keep composing in one place.
// Safe (a no-op) before scheduler_init().
void scheduler_reapply_brightness(void);
