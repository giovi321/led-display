#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SCHED_DAYS_MON  (1 << 0)
#define SCHED_DAYS_TUE  (1 << 1)
#define SCHED_DAYS_WED  (1 << 2)
#define SCHED_DAYS_THU  (1 << 3)
#define SCHED_DAYS_FRI  (1 << 4)
#define SCHED_DAYS_SAT  (1 << 5)
#define SCHED_DAYS_SUN  (1 << 6)
#define SCHED_DAYS_ALL  0x7F

// What happens outside the on-window. Stored in NVS as a plain u8; keep the
// values stable.
typedef enum {
    SCHED_OFF_ACT_OFF = 0,   // blank the panel (brightness 0) - the default
    SCHED_OFF_ACT_DIM = 1,   // keep showing, at off_brightness
} sched_off_action_t;

#define SCHED_OFF_BRI_DEFAULT  8

// POSIX TZ string, e.g. "CET-1CEST,M3.5.0,M10.5.0/3" for Europe/Zurich.
#define SCHED_TZ_MAX  48

typedef struct {
    bool    enabled;
    uint8_t on_hour;
    uint8_t on_min;
    uint8_t off_hour;
    uint8_t off_min;
    uint8_t days;           // bitmask: bit0=Mon .. bit6=Sun
    int16_t utc_offset;     // minutes from UTC (e.g. +60 for CET)
    uint8_t off_action;     // sched_off_action_t
    uint8_t off_brightness; // 1..255, applied when off_action = DIM
    // Timezone override. Empty = the fixed utc_offset above (historical
    // behaviour, refreshed from the browser on every save). When set, the
    // schedule follows this POSIX TZ rule instead - DST transitions included.
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
// off-window, the sensor and the staleness dim keep composing in one place.
// Safe (a no-op) before scheduler_init().
void scheduler_reapply_brightness(void);
