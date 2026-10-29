// /api/value/poll and /api/value/poll/now (see api_handlers.h for the
// ownership table, value_poll.h for the poller).
//
// Secrets policy: the GET stays open, so it reports the custom header
// value only as a *_set boolean. Neither POST blocks on the
// network: saving and "poll now" wake the poller task and return at once;
// the UI re-reads GET /api/value/poll until last_poll changes.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "api_handlers.h"
#include "value_poll.h"
#include "web_common.h"

static const char *TAG = "api_vpoll";

static void add_status(cJSON *parent)
{
    value_poll_status_t st;
    value_poll_get_status(&st);

    cJSON *s = cJSON_AddObjectToObject(parent, "status");
    cJSON_AddNumberToObject(s, "last_poll", (double)st.last_poll);
    cJSON_AddNumberToObject(s, "http_status", st.http_status);
    cJSON_AddStringToObject(s, "raw", st.raw);
    cJSON_AddStringToObject(s, "error", st.error);
    cJSON_AddNumberToObject(s, "failures", st.failures);
}

static esp_err_t vpoll_get_handler(httpd_req_t *req)
{
    value_poll_cfg_t cfg;
    value_poll_get(&cfg);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", cfg.enabled);
    cJSON_AddStringToObject(r, "url", cfg.url);
    cJSON_AddNumberToObject(r, "interval_s", cfg.interval_s);
    cJSON_AddStringToObject(r, "hdr_name", cfg.hdr_name);
    cJSON_AddBoolToObject(r, "hdr_value_set", cfg.hdr_value[0] != '\0');
    cJSON_AddStringToObject(r, "path", cfg.path);
    add_status(r);
    memset(&cfg, 0, sizeof(cfg));
    return web_send_json(req, r);
}

// Copy a present string field; false when it is too long for `sz`.
static bool take_str(const cJSON *body, const char *key, char *dst, size_t sz)
{
    const cJSON *it = cJSON_GetObjectItem(body, key);
    if (!cJSON_IsString(it)) return true;
    if (strlen(it->valuestring) >= sz) return false;
    snprintf(dst, sz, "%s", it->valuestring);
    return true;
}

// Like take_str, but an absent OR empty field keeps the stored value: a
// secret is only removed by an explicit clear_* flag.
static bool take_secret(const cJSON *body, const char *key, char *dst, size_t sz)
{
    const cJSON *it = cJSON_GetObjectItem(body, key);
    if (!cJSON_IsString(it) || it->valuestring[0] == '\0') return true;
    if (strlen(it->valuestring) >= sz) return false;
    snprintf(dst, sz, "%s", it->valuestring);
    return true;
}

static esp_err_t vpoll_post_handler(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *body = web_read_json(req, 2048);
    if (!body) return web_send_err(req, "invalid JSON body", 400);

    value_poll_cfg_t cfg;
    value_poll_get(&cfg);
    const char *bad = NULL;

    const cJSON *it;
    if (cJSON_IsBool(it = cJSON_GetObjectItem(body, "enabled"))) {
        cfg.enabled = cJSON_IsTrue(it);
    }
    if ((it = cJSON_GetObjectItem(body, "interval_s")) != NULL) {
        if (!cJSON_IsNumber(it) || it->valuedouble < VPOLL_INTERVAL_MIN ||
            it->valuedouble > VPOLL_INTERVAL_MAX) {
            bad = "interval_s must be 10..86400";
        } else {
            cfg.interval_s = (uint32_t)it->valuedouble;
        }
    }
    // Clears first, so one request can clear and set a new name or value.
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "clear_header"))) {
        cfg.hdr_name[0]  = '\0';
        cfg.hdr_value[0] = '\0';
    }
    if (!bad && (!take_str(body, "url", cfg.url, sizeof(cfg.url)) ||
                 !take_str(body, "hdr_name", cfg.hdr_name, sizeof(cfg.hdr_name)) ||
                 !take_str(body, "path", cfg.path, sizeof(cfg.path)))) {
        bad = "field too long";
    }
    if (!bad && !take_secret(body, "hdr_value", cfg.hdr_value, sizeof(cfg.hdr_value))) {
        bad = "field too long";
    }
    cJSON_Delete(body);

    if (!bad) bad = value_poll_validate(&cfg);
    if (!bad && cfg.hdr_value[0] != '\0' && cfg.hdr_name[0] == '\0') {
        bad = "hdr_value requires hdr_name";
    }
    if (bad) {
        memset(&cfg, 0, sizeof(cfg));
        return web_send_err(req, bad, 400);
    }

    esp_err_t err = value_poll_set(&cfg);   // saves, then wakes the poller
    memset(&cfg, 0, sizeof(cfg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving value poll config failed: %s", esp_err_to_name(err));
        return web_send_err(req, "saving value poll config failed", 500);
    }
    return web_send_ok(req);
}

static esp_err_t vpoll_now_handler(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    // Never waits for the result: the poll runs on the poller task and a
    // blocking handler would stall the single httpd task.
    bool pending = value_poll_ready();
    if (pending) value_poll_now();

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddBoolToObject(r, "pending", pending);
    add_status(r);
    return web_send_json(req, r);
}

void api_value_poll_register(httpd_handle_t srv)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/value/poll",     .method = HTTP_GET,  .handler = vpoll_get_handler},
        {.uri = "/api/value/poll",     .method = HTTP_POST, .handler = vpoll_post_handler},
        {.uri = "/api/value/poll/now", .method = HTTP_POST, .handler = vpoll_now_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        web_register_uri(srv, &uris[i]);
    }
}
