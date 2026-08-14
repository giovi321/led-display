// Outbound event webhooks (DESIGN.md section 6): fire-and-forget JSON POSTs
// delivered from a low-priority worker task so HTTP handlers and the
// scheduler tick never block on the network. Queue depth 8; when it is full
// the newest event is dropped with a log line - losing a webhook must never
// stall the caller.

#include "webhooks.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "webhooks";

#define WEBHOOK_QUEUE_DEPTH     8
#define WEBHOOK_TIMEOUT_MS      5000
#define WEBHOOK_RETRY_DELAY_MS  2000
// cJSON printing + esp_http_client (TLS-capable) + logging: 4 KB left too
// little margin under DNS-failure paths; the extra heap is a one-time cost.
#define WEBHOOK_TASK_STACK      8192
#define WEBHOOK_TASK_PRIO       3

typedef struct {
    uint32_t event_bit;
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
        default:                       return "unknown";
    }
}

// One POST attempt. Success = transport OK and a 2xx status.
static bool post_once(const char *url, const char *payload)
{
    esp_http_client_config_t cfg = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = WEBHOOK_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "Content-Type", "application/json");
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

static void webhook_task(void *arg)
{
    (void)arg;
    webhook_msg_t msg;

    while (1) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) continue;

        // Re-read the config at delivery time: the URL or device name may
        // have changed while the event sat in the queue.
        integrations_cfg_t cfg;
        integrations_get(&cfg);
        if (cfg.webhook_url[0] == '\0') {
            free(msg.json_data);
            continue;
        }

        cJSON *root = cJSON_CreateObject();
        if (!root) {
            free(msg.json_data);
            continue;
        }
        cJSON_AddStringToObject(root, "event", event_name(msg.event_bit));
        cJSON_AddStringToObject(root, "device", cfg.device_name);
        cJSON_AddNumberToObject(root, "uptime_s",
                                (double)(esp_timer_get_time() / 1000000));
        if (msg.json_data) {
            cJSON *data = cJSON_Parse(msg.json_data);
            if (data) {
                cJSON_AddItemToObject(root, "data", data);
            }
            // Unparseable data is simply omitted: the envelope stays valid.
        }
        free(msg.json_data);

        char *payload = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        if (!payload) continue;

        bool ok = post_once(cfg.webhook_url, payload);
        if (!ok) {
            // One retry after 2 s: enough for a webhook receiver that was
            // mid-restart, no ambition beyond that (fire-and-forget).
            vTaskDelay(pdMS_TO_TICKS(WEBHOOK_RETRY_DELAY_MS));
            ok = post_once(cfg.webhook_url, payload);
        }
        cJSON_free(payload);

        if (ok) {
            ESP_LOGI(TAG, "delivered '%s'", event_name(msg.event_bit));
        } else {
            ESP_LOGW(TAG, "giving up on '%s' after retry",
                     event_name(msg.event_bit));
        }
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
    // this config read.
    integrations_cfg_t cfg;
    integrations_get(&cfg);
    if (cfg.webhook_url[0] == '\0' || !(cfg.webhook_events & event_bit)) {
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
