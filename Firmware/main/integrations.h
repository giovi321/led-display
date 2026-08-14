#pragma once

// Integration settings: API key, outbound webhook target, device name and
// the value-endpoint formatting/staleness config. Persisted as JSON blobs
// via settings.c; cached in RAM for cheap reads from HTTP handlers and the
// scheduler tick.

#include <stdbool.h>
#include <stddef.h>
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
    // The single device name (see devname.h): also the network hostname and the
    // setup-AP SSID. Stored under its own NVS key; this is the cached copy the
    // webhook worker reads.
    char     device_name[INTEGR_NAME_MAX + 1];
} integrations_cfg_t;

esp_err_t integrations_init(void);   // load blobs from NVS into the cache

void integrations_get(integrations_cfg_t *out);
esp_err_t integrations_set(const integrations_cfg_t *in);   // save + recache

// ---- device name -----------------------------------------------------------

void integrations_get_device_name(char *buf, size_t bufsize);

// Store a new device name and apply it everywhere it is used: webhook payloads
// (immediately), the netif hostname (on the next DHCP renew or reboot) and the
// setup-AP SSID (next time that AP is raised). Names are cleaned to the
// devname.h charset; "" or the default resets to the historical defaults.
esp_err_t integrations_set_device_name(const char *name);

void integrations_get_value_cfg(value_cfg_t *out);
esp_err_t integrations_set_value_cfg(const value_cfg_t *in);

// Value freshness bookkeeping (used by /api/value and the staleness tick).
void integrations_touch_value(void);       // record "a value arrived now"
int64_t integrations_value_age_s(void);    // seconds since last value, -1 = never

// Last formatted value, RAM only (does not survive a reboot - that is what
// the value config's default_text placeholder is for). Written by every
// value push whatever the display source, so switching static ->
// integration can show the latest value immediately.
void integrations_set_last_value(const char *text);
bool integrations_get_last_value(char *buf, size_t bufsize);  // false = none yet
