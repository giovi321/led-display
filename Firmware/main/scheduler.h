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

typedef struct {
    bool    enabled;
    uint8_t on_hour;
    uint8_t on_min;
    uint8_t off_hour;
    uint8_t off_min;
    uint8_t days;       // bitmask: bit0=Mon .. bit6=Sun
    int16_t utc_offset; // minutes from UTC (e.g. +60 for CET)
} schedule_t;

esp_err_t scheduler_init(void);

void scheduler_get(schedule_t *out);
esp_err_t scheduler_set(const schedule_t *in);

bool scheduler_display_is_off(void);
