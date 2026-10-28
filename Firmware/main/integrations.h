#pragma once

// Integration settings: API key, outbound webhook target, device name and
// the value-endpoint formatting/fallback config. Persisted as JSON blobs
// via settings.c; cached in RAM for cheap reads from HTTP handlers and the
// scheduler tick.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "valuefmt.h"   // value_cfg_t, fallback_cfg_t (pure headers)

struct cJSON;

#define INTEGR_API_KEY_MAX     64   // plus NUL
#define INTEGR_URL_MAX         160  // plus NUL
#define INTEGR_NAME_MAX        32   // plus NUL
#define INTEGR_UI_USER_MAX     32   // plus NUL
#define INTEGR_UI_PASS_MAX     64   // plus NUL
#define INTEGR_SECRET_MAX      128  // plus NUL: webhook secret, header value
#define INTEGR_HDR_NAME_MAX    48   // plus NUL
#define INTEGR_HEARTBEAT_MAX   1440 // minutes

// Outbound webhook event bits.
#define WEBHOOK_EVT_BOOT         (1u << 0)
#define WEBHOOK_EVT_IP_CHANGE    (1u << 1)
#define WEBHOOK_EVT_OTA_RESULT   (1u << 2)
#define WEBHOOK_EVT_SCHEDULE_ON  (1u << 3)
#define WEBHOOK_EVT_SCHEDULE_OFF (1u << 4)
#define WEBHOOK_EVT_VALUE_STALE  (1u << 5)
#define WEBHOOK_EVT_VALUE_RECOVERED (1u << 6)
// Heartbeat only: driven by heartbeat_min, never selectable in webhook_events.
#define WEBHOOK_EVT_STATUS       (1u << 7)
// Every bit a user may set in webhook_events.
#define WEBHOOK_EVT_SELECTABLE   ((1u << 7) - 1u)

typedef struct {
    char     api_key[INTEGR_API_KEY_MAX + 1];   // empty = auth disabled
    char     webhook_url[INTEGR_URL_MAX + 1];   // empty = webhooks disabled
    uint32_t webhook_events;                    // WEBHOOK_EVT_* mask
    // The single device name (see devname.h): also the network hostname and the
    // setup-AP SSID. Stored under its own NVS key; this is the cached copy the
    // webhook worker reads.
    char     device_name[INTEGR_NAME_MAX + 1];
    // Browser login for the web UI: HTTP Basic over the LAN, off by default.
    // Separate from api_key, which gates machine callers by header and says
    // nothing to a browser. Enabling needs a non-empty password; the pair is
    // ignored while ui_auth is false so turning it off never loses it.
    bool     ui_auth;
    char     ui_user[INTEGR_UI_USER_MAX + 1];
    char     ui_pass[INTEGR_UI_PASS_MAX + 1];
    // Outbound webhook auth: secret sent as "Authorization: Bearer", plus one
    // optional custom header. Secrets: never logged, GET reports *_set only.
    char     webhook_secret[INTEGR_SECRET_MAX + 1];
    char     webhook_hdr_name[INTEGR_HDR_NAME_MAX + 1];
    char     webhook_hdr_value[INTEGR_SECRET_MAX + 1];
    uint16_t heartbeat_min;                     // 0 = off, 1..1440
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

// Overlay the value-config keys present in `obj` onto `v` (absent keys keep
// their value). Accepts the current keys and the pre-2.12 legacy keys
// (stale_hours, stale_action, stale_text, default_text), which map through
// fb_migrate_legacy; when both appear, the fb_* keys win.
//   strict = true  (API): a bad value fails with *err set to the 400 message
//                         and `v` must then be discarded.
//   strict = false (blob load, backup import): bad values are clamped or
//                         ignored and a separator clash is resolved; never fails.
bool integrations_value_cfg_from_json(value_cfg_t *v, const struct cJSON *obj,
                                      bool strict, const char **err);

// Add every value-config key of `v` to `obj` (the v2 blob / API shape).
// legacy = true also adds the derived pre-2.12 keys for old clients.
void integrations_value_cfg_to_json(const value_cfg_t *v, struct cJSON *obj,
                                    bool legacy);

// Value freshness bookkeeping (written by value_feed, read for reports).
void integrations_touch_value(void);       // record "a value arrived now"
int64_t integrations_value_age_s(void);    // seconds since last value, -1 = never

// Last formatted value, RAM only (does not survive a reboot - that is what
// the value config's fallback text is for). Written by every
// value push whatever the display source, so switching static ->
// integration can show the latest value immediately.
void integrations_set_last_value(const char *text);
bool integrations_get_last_value(char *buf, size_t bufsize);  // false = none yet
