#pragma once

// URL value poller: fetches one value from an HTTP(S) URL on an interval and
// hands it to value_feed_ingest() exactly like POST /api/value would.
//
// Runs on its own low-priority task, never on the HTTP server or display
// task. Polls only while enabled, with a URL set, and while WiFi is in
// station mode with an IP. HTTPS is verified against the IDF CA bundle.
//
// Persisted as the "vpoll" JSON blob via settings.c. The secret, the header
// value and the URL (which may carry credentials) are never logged.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "jsonpath.h"

#define VPOLL_URL_MAX       255
#define VPOLL_SECRET_MAX    128
#define VPOLL_HDR_NAME_MAX  48
#define VPOLL_BODY_MAX      4096
#define VPOLL_TIMEOUT_MS    10000

#define VPOLL_INTERVAL_MIN      10
#define VPOLL_INTERVAL_MAX      86400
#define VPOLL_INTERVAL_DEFAULT  300

typedef struct {
    bool     enabled;
    char     url[VPOLL_URL_MAX + 1];
    uint32_t interval_s;                    // 10..86400, default 300
    char     secret[VPOLL_SECRET_MAX + 1];  // "Authorization: Bearer <secret>"
    char     hdr_name[VPOLL_HDR_NAME_MAX + 1];
    char     hdr_value[VPOLL_SECRET_MAX + 1];
    char     path[JSONPATH_PATH_MAX + 1];   // "" = whole trimmed body
} value_poll_cfg_t;

typedef struct {
    int64_t  last_poll;     // unix seconds, 0 = never
    int      http_status;   // 0 = no response
    char     raw[64];       // extracted value (truncated copy, display only)
    char     error[64];     // "" on success
    uint16_t failures;      // consecutive failed polls
} value_poll_status_t;

// Load the blob and start the poller task. Call after ota_mark_valid().
esp_err_t value_poll_init(void);

void value_poll_get(value_poll_cfg_t *out);

// Validate, save the "vpoll" blob, recache, wake the task for one poll.
// ESP_ERR_INVALID_ARG when the config is out of range (see
// value_poll_validate).
esp_err_t value_poll_set(const value_poll_cfg_t *in);

// NULL when `c` is acceptable, else the reason (API error text).
const char *value_poll_validate(const value_poll_cfg_t *c);

void value_poll_get_status(value_poll_status_t *out);

// Wake the task: it polls at once when polling can run (see value_poll_ready).
void value_poll_now(void);

// True when a poll would run now: enabled, URL set, station mode with an IP.
bool value_poll_ready(void);
