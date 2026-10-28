// Outbound event webhooks (DESIGN.md section 6): fire-and-forget JSON POSTs
// delivered from a low-priority worker task so HTTP handlers and the
// scheduler tick never block on the network. Queue depth 8; when it is full
// the newest event is dropped with a log line - losing a webhook must never
// stall the caller.
//
// The same task owns the status heartbeat: it waits on the queue with a
// timeout equal to the time left until the next heartbeat and posts "status"
// on timeout, independent of webhook_events.

#include "webhooks.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "display.h"
#include "fallback.h"
#include "fw_version.h"
#include "http_auth.h"
#include "settings.h"
#include "value_feed.h"
#include "wifi_manager.h"

static const char *TAG = "webhooks";

#define WEBHOOK_QUEUE_DEPTH     8
#define WEBHOOK_TIMEOUT_MS      5000
#define WEBHOOK_RETRY_DELAY_MS  2000
// cJSON printing + esp_http_client (TLS-capable) + logging: 4 KB left too
// little margin under DNS-failure paths; the extra heap is a one-time cost.
// +2 KB over the previous 8 KB for the status heartbeat, which keeps a
// settings_t, the panel text buffer and a wifi status on the stack while it
// builds its JSON.
#define WEBHOOK_TASK_STACK      10240
#define WEBHOOK_TASK_PRIO       3

typedef struct {
    uint32_t event_bit;   // 0 = wake message (re-read heartbeat_min)
    char    *json_data;   // heap copy of the caller's "data" object, may be NULL
} webhook_msg_t;

static QueueHandle_t s_queue;

// Local strdup: keeps the module strict-ISO clean (strdup is POSIX).
static char *dup_string(const char *s)
{
    size_t n = strlen(s) + 1;
    char *copy = malloc(n);
    if (copy) memcpy(copy, s, n);
    return copy;
}

static const char *event_name(uint32_t bit)
{
    switch (bit) {
        case WEBHOOK_EVT_BOOT:         return "boot";
        case WEBHOOK_EVT_IP_CHANGE:    return "ip_change";
        case WEBHOOK_EVT_OTA_RESULT:   return "ota_result";
        case WEBHOOK_EVT_SCHEDULE_ON:  return "schedule_on";
        case WEBHOOK_EVT_SCHEDULE_OFF: return "schedule_off";
        case WEBHOOK_EVT_VALUE_STALE:  return "value_stale";
        case WEBHOOK_EVT_VALUE_RECOVERED: return "value_recovered";
        case WEBHOOK_EVT_STATUS:       return "status";
        default:                      return "unknown";
    }
}

// One POST attempt. Success = transport OK and a 2xx status.
static bool post_once(const integrations_cfg_t *icfg, const char *payload)
{
    esp_http_client_config_t cfg = {
        .url            = icfg->webhook_url,
        .method         = HTTP_METHOD_POST,
        .timeout_ms     = WEBHOOK_TIMEOUT_MS,
        .buffer_size    = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    http_auth_apply(client, icfg->webhook_secret, icfg->webhook_hdr_name,
                    icfg->webhook_hdr_value);
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "POST rejected: HTTP %d", status);
        return false;
    }
    return true;
}

// Data object of the heartbeat "status" event as a printed JSON string (NULL
// on OOM). Caller frees with cJSON_free().
static char *build_status_data(void)
{
    char text[DISPLAY_TEXT_MAX];
    display_get_state(text, sizeof(text), NULL, NULL, NULL, NULL);

    settings_t st;
    settings_load(&st);

    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);

    value_feed_state_t fb;
    value_feed_get_state(&fb);

    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    cJSON_AddStringToObject(d, "ip", w.ip);
    cJSON_AddNumberToObject(d, "rssi", w.rssi);
    cJSON_AddNumberToObject(d, "uptime_s",
                            (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(d, "fw", FW_VERSION);
    cJSON_AddNumberToObject(d, "brightness", st.brightness);
    cJSON_AddStringToObject(d, "power", text[0] != '\0' ? "on" : "off");
    cJSON_AddStringToObject(d, "source",
                            st.source == DISPLAY_SRC_INTEGRATION
                                ? "integration" : "static");
    cJSON_AddStringToObject(d, "text", text);
    cJSON_AddNumberToObject(d, "value_age_s",
                            (double)integrations_value_age_s());
    // Same rule as MQTT: no_value before the first value is information,
    // not a fault, so it reports fallback false with the reason still set.
    cJSON_AddBoolToObject(d, "fallback", fb.active && fb_notifies(fb.reason));
    cJSON_AddStringToObject(d, "fallback_reason",
                            fb.active ? fb_reason_str(fb.reason) : "none");
    char *out = cJSON_PrintUnformatted(d);
    cJSON_Delete(d);
    return out;
}

// Wrap `json_data` (may be NULL) in the event envelope and deliver it with
// one retry. Takes ownership of json_data.
static void deliver(const integrations_cfg_t *cfg, uint32_t event_bit,
                    char *json_data)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        free(json_data);
        return;
    }
    cJSON_AddStringToObject(root, "event", event_name(event_bit));
    cJSON_AddStringToObject(root, "device", cfg->device_name);
    cJSON_AddNumberToObject(root, "uptime_s",
                            (double)(esp_timer_get_time() / 1000000));
    if (json_data) {
        cJSON *data = cJSON_Parse(json_data);
        if (data) {
            cJSON_AddItemToObject(root, "data", data);
        }
        // Unparseable data is simply omitted: the envelope stays valid.
    }
    free(json_data);

    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) return;

    bool ok = post_once(cfg, payload);
    if (!ok) {
        // One retry after 2 s: enough for a webhook receiver that was
        // mid-restart, no ambition beyond that (fire-and-forget).
        vTaskDelay(pdMS_TO_TICKS(WEBHOOK_RETRY_DELAY_MS));
        ok = post_once(cfg, payload);
    }
    cJSON_free(payload);

    if (ok) {
        ESP_LOGI(TAG, "delivered '%s'", event_name(event_bit));
    } else {
        ESP_LOGW(TAG, "giving up on '%s' after retry", event_name(event_bit));
    }
}

static void webhook_task(void *arg)
{
    (void)arg;
    webhook_msg_t msg;
    // Absolute time of the next heartbeat in us; 0 = (re)compute from now.
    int64_t next_hb_us = 0;

    while (1) {
        // Read here only to size the wait; re-read after the wait below.
        integrations_cfg_t cfg;
        integrations_get(&cfg);

        TickType_t wait = portMAX_DELAY;
        if (cfg.heartbeat_min > 0) {
            int64_t now = esp_timer_get_time();
            if (next_hb_us == 0) {
                next_hb_us = now + (int64_t)cfg.heartbeat_min * 60 * 1000000;
            }
            int64_t left_ms = (next_hb_us - now) / 1000;
            wait = left_ms <= 0
                       ? 0
                       : (TickType_t)((uint64_t)left_ms * configTICK_RATE_HZ / 1000);
        } else {
            next_hb_us = 0;
        }

        bool got = xQueueReceive(s_queue, &msg, wait) == pdTRUE;

        // The pre-wait read only sized the wait and can be hours old: re-read
        // so delivery uses the current URL, auth and device name.
        integrations_get(&cfg);

        if (!got) {
            // Timeout: the heartbeat is due. Sent regardless of
            // webhook_events; only an empty URL silences it.
            if (cfg.heartbeat_min > 0) {
                next_hb_us = esp_timer_get_time() +
                             (int64_t)cfg.heartbeat_min * 60 * 1000000;
                if (cfg.webhook_url[0] != '\0') {
                    deliver(&cfg, WEBHOOK_EVT_STATUS, build_status_data());
                }
            }
            continue;
        }

        if (msg.event_bit == 0) {   // wake: heartbeat_min may have changed
            next_hb_us = 0;
            continue;
        }

        if (cfg.webhook_url[0] == '\0') {
            free(msg.json_data);
            continue;
        }
        deliver(&cfg, msg.event_bit, msg.json_data);
    }
}

esp_err_t webhooks_init(void)
{
    if (s_queue) return ESP_OK;   // already running

    s_queue = xQueueCreate(WEBHOOK_QUEUE_DEPTH, sizeof(webhook_msg_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    if (xTaskCreate(webhook_task, "webhooks", WEBHOOK_TASK_STACK, NULL,
                    WEBHOOK_TASK_PRIO, NULL) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "worker started (queue depth %d)", WEBHOOK_QUEUE_DEPTH);
    return ESP_OK;
}

void webhooks_fire(uint32_t event_bit, const char *json_data)
{
    if (!s_queue) return;   // called before webhooks_init(): safe no-op

    // Filter here (not in the worker) so disabled events cost nothing but
    // this config read. The heartbeat "status" never comes through here: the
    // worker posts it itself, bypassing webhook_events.
    integrations_cfg_t cfg;
    integrations_get(&cfg);
    if (cfg.webhook_url[0] == '\0' ||
        (event_bit != WEBHOOK_EVT_STATUS && !(cfg.webhook_events & event_bit))) {
        return;
    }

    webhook_msg_t msg = {.event_bit = event_bit, .json_data = NULL};
    if (json_data) {
        msg.json_data = dup_string(json_data);
        if (!msg.json_data) return;   // OOM: drop rather than block
    }

    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full, dropping '%s'", event_name(event_bit));
        free(msg.json_data);
    }
}

void webhooks_wake(void)
{
    if (!s_queue) return;
    webhook_msg_t msg = {.event_bit = 0, .json_data = NULL};
    // A full queue means the worker is busy and re-reads the config after
    // every message, so a dropped wake only delays the interval restart.
    xQueueSend(s_queue, &msg, 0);
}
