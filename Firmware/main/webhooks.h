#pragma once

// Outbound event webhooks: fire-and-forget JSON POSTs to the configured URL
// (integrations_cfg_t.webhook_url) from a low-priority worker task, one
// retry, 5 s timeout. Payload shape:
//   {"event":"boot","device":"<name>","uptime_s":12,"data":{...}}

#include "esp_err.h"

#include "integrations.h"

esp_err_t webhooks_init(void);

// Queue an event. `json_data` is an optional cJSON-printed object string
// merged in as "data" (the module copies it; caller keeps ownership). Does
// nothing when the event bit is off or no URL is configured.
void webhooks_fire(uint32_t event_bit, const char *json_data);

// Wake the worker so it re-reads heartbeat_min and restarts the status
// interval. Call after the integrations config changed.
void webhooks_wake(void);
