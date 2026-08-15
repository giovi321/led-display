#pragma once

// The schedule's pure decision: given the day mask, the on/off times, the
// rule for days outside the mask and a local wall-clock instant, does the
// off-action apply right now?
//
// Split out of scheduler.c (which needs NVS, SNTP and the LED driver) so the
// part that is easy to get wrong is plain C and testable on the host, the
// same way light_curve.c is split out of light_sensor.c.

#include <stdbool.h>
#include <stdint.h>

// Day bitmask. Bit 0 is Monday so that the mask reads left to right the way
// the week is written; struct tm does not, hence sched_day_bit().
#define SCHED_DAYS_MON  (1 << 0)
#define SCHED_DAYS_TUE  (1 << 1)
#define SCHED_DAYS_WED  (1 << 2)
#define SCHED_DAYS_THU  (1 << 3)
#define SCHED_DAYS_FRI  (1 << 4)
#define SCHED_DAYS_SAT  (1 << 5)
#define SCHED_DAYS_SUN  (1 << 6)
#define SCHED_DAYS_ALL  0x7F

// What a day NOT in the mask does. Stored in NVS as a plain u8; keep the
// values stable.
typedef enum {
    // One long off-window: the off-action holds all day. Unticking Saturday
    // and Sunday is how you keep a display dark over the weekend. The
    // default, and the only behaviour before 2.8.0.
    SCHED_OTHER_OFF = 0,
    // The schedule does not apply at all: the panel runs at its normal
    // brightness around the clock, as if the schedule were disabled.
    SCHED_OTHER_RUN = 1,
} sched_other_days_t;

typedef struct {
    uint8_t days;        // SCHED_DAYS_* bitmask
    uint8_t on_hour;
    uint8_t on_min;
    uint8_t off_hour;
    uint8_t off_min;
    uint8_t other_days;  // sched_other_days_t
} sched_window_t;

// struct tm's tm_wday (0 = Sunday .. 6 = Saturday) to a SCHED_DAYS_* bit.
// Returns 0 for a value outside 0..6, which matches no day and so reads as
// "outside the mask".
int sched_day_bit(int tm_wday);

// Whether the on-window covers this instant. A window whose off time is at or
// before its on time wraps midnight (on 22:00, off 06:00), but it does not
// carry into the next day unless that day is in the mask too.
bool sched_in_window(const sched_window_t *w, int tm_wday, int hour, int min);

// Whether the schedule holds the panel down right now - the question
// evaluate_window() actually asks. Differs from !sched_in_window() only on a
// day outside the mask, where SCHED_OTHER_RUN hands the panel back.
bool sched_holds_panel_down(const sched_window_t *w, int tm_wday,
                            int hour, int min);
