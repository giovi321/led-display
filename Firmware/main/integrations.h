#pragma once

// Integration settings: API key, outbound webhook target, device name and
// the value-endpoint formatting/staleness config. Persisted as JSON blobs
// via settings.c; cached in RAM for cheap reads from HTTP handlers and the
// scheduler tick.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "valuefmt.h"   // value_cfg_t, stale_action_t (pure header)

#define INTEGR_API_KEY_MAX     64   // plus NUL
#define INTEGR_URL_MAX         160  // plus NUL
#define INTEGR_NAME_MAX        32   // plus NUL

// Outbound webhook event bits.
#define WEBHOOK_EVT_BOOT         (1u << 0)
#define WEBHOOK_EVT_IP_CHANGE    (1u << 1)
#define WEBHOOK_EVT_OTA_RESULT   (1u << 2)
#define WEBHOOK_EVT_SCHEDULE_ON  (1u << 3)
#define WEBHOOK_EVT_SCHEDULE_OFF (1u << 4)
#define WEBHOOK_EVT_VALUE_STALE  (1u << 5)

typedef struct {
    char     api_key[INTEGR_API_KEY_MAX + 1];   // empty = auth disabled
    char     webhook_url[INTEGR_URL_MAX + 1];   // empty = webhooks disabled
    uint32_t webhook_events;                    // WEBHOOK_EVT_* mask
    char     device_name[INTEGR_NAME_MAX + 1];  // reported in webhook payloads
} integrations_cfg_t;

esp_err_t integrations_init(void);   // load blobs from NVS into the cache

void integrations_get(integrations_cfg_t *out);
esp_err_t integrations_set(const integrations_cfg_t *in);   // save + recache

void integrations_get_value_cfg(value_cfg_t *out);
esp_err_t integrations_set_value_cfg(const value_cfg_t *in);

// Value freshness bookkeeping (used by /api/value and the staleness tick).
void integrations_touch_value(void);       // record "a value arrived now"
int64_t integrations_value_age_s(void);    // seconds since last value, -1 = never
