#pragma once

// Fallback state machine: decides when the panel shows a fallback instead of
// the value (no value since boot, value timeout, consecutive poll failures,
// MQTT broker lost). PURE C (no ESP-IDF, no clock): every call takes the
// current time in seconds, so it is host-testable with a fake clock.
//
// Triggers are events, not conditions re-checked each tick. A value from any
// source clears the fallback, resets the poll failure count and re-arms the
// triggers.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FB_MODE_TEXT = 0, FB_MODE_MARKER = 1, FB_MODE_DIM = 2 } fb_mode_t;

typedef enum {
    FB_REASON_NONE = 0,
    FB_REASON_NO_VALUE,
    FB_REASON_TIMEOUT,
    FB_REASON_POLL_FAILURES,
    FB_REASON_MQTT_LOST,
} fb_reason_t;

typedef enum {
    FB_CHANGE_NONE = 0,
    FB_CHANGE_ENGAGED,
    FB_CHANGE_CLEARED,
} fb_change_t;

#define FB_TEXT_MAX      64
#define FB_MQTT_GRACE_S  60
#define FB_AFTER_MIN_MAX 43200

typedef struct {
    char      text[FB_TEXT_MAX + 1];
    fb_mode_t mode;
    uint16_t  after_min;       // 0 = off, 1..43200
    uint8_t   poll_failures;   // 0 = off, default 3
    bool      on_mqtt_loss;    // default false
} fallback_cfg_t;

typedef struct {
    bool        active;
    fb_reason_t reason;
    bool        have_value;
    int64_t     last_value_s;      // or boot time when no value yet
    uint16_t    poll_fail_count;
    bool        poll_armed, mqtt_armed;
    int64_t     mqtt_down_since_s; // -1 = connected / unknown
} fb_state_t;

// Active with reason NO_VALUE.
void        fb_init(fb_state_t *s, int64_t now_s);

// A value arrived from any source: clears, resets the poll counter, re-arms.
fb_change_t fb_on_value(fb_state_t *s, int64_t now_s);

// A poll finished. ok behaves like a value; a failure counts toward the
// threshold and engages once when the count reaches it.
fb_change_t fb_on_poll_result(fb_state_t *s, const fallback_cfg_t *c, bool ok,
                              int64_t now_s);

// MQTT connect/disconnect. Only records state; the trigger fires in fb_tick.
fb_change_t fb_on_mqtt(fb_state_t *s, bool connected, int64_t now_s);

// Periodic evaluation of the timeout and MQTT-loss triggers.
fb_change_t fb_tick(fb_state_t *s, const fallback_cfg_t *c, bool mqtt_enabled,
                    int64_t now_s);

bool        fb_notifies(fb_reason_t r);          // false for NONE and NO_VALUE
const char *fb_reason_str(fb_reason_t r);        // "no_value","timeout","poll_failures","mqtt_lost"
const char *fb_mode_str(fb_mode_t m);            // "text","marker","dim"
bool        fb_mode_from_str(const char *s, fb_mode_t *out);
void        fb_cfg_defaults(fallback_cfg_t *c);

// Legacy (<=2.11.0) settings -> new config. fallback text = default_text if
// non-empty else stale_text; after_min = stale_hours * 60 (clamped); mode from
// stale_action; action none -> after_min 0 and mode text.
void        fb_migrate_legacy(fallback_cfg_t *out, uint16_t stale_hours,
                              int stale_action /*0 none,1 dim,2 marker,3 text*/,
                              const char *stale_text, const char *default_text);

#ifdef __cplusplus
}
#endif
