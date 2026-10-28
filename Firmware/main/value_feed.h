#pragma once

// value_feed: the one ingest path for every value source (POST /api/value,
// MQTT {base}/value/set, the URL poller) and the glue between those sources,
// the fallback state machine (fallback.c) and the panel.
//
// Fallback evaluation and reporting (webhooks, MQTT state) run whatever the
// display source; the panel itself (fallback text, corner marker, dim) only
// changes while the display source is "integration".
//
// Locking: every fb_* call and every panel write it decides happen under one
// internal mutex. Webhooks, MQTT state publishes and brightness re-applies run
// only after that mutex is released.

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#include "fallback.h"

typedef enum { VALUE_SRC_HTTP = 0, VALUE_SRC_MQTT, VALUE_SRC_POLL } value_src_t;

typedef struct {
    bool        active;
    fb_reason_t reason;
    fb_mode_t   mode;
} value_feed_state_t;

esp_err_t value_feed_init(void);

// Format, store as last value, record arrival, clear fallback, render when
// the display source is integration. `out` gets the formatted text.
void value_feed_ingest(const char *raw, value_src_t src, char *out, size_t outsz);

// A poll finished without a value: counts toward the poll-failure trigger.
void value_feed_poll_failed(void);

// MQTT broker connected / disconnected (also: link disabled = connected, so a
// later disconnect starts a fresh grace period).
void value_feed_mqtt_state(bool connected);

// Periodic evaluation (timeout and MQTT-loss triggers, marker re-poke).
// Called by the scheduler every 30 s. Does nothing in setup-AP mode.
void value_feed_tick(void);

void value_feed_get_state(value_feed_state_t *out);

// True while the fallback dim halves the brightness (scheduler arbiter).
// Lock-free: safe to call with the scheduler's arbiter mutex held.
bool value_feed_dim_active(void);

// Re-render the integration panel from the current fallback state (after a
// display source switch or a fallback config change) and re-apply the
// brightness. With the static source only the brightness is re-applied.
void value_feed_apply_panel(void);
