// URL value poller (see value_poll.h).
//
// One task, woken by a task notification (config saved, "poll now") or by
// the end of its wait: the configured interval while polling can run, a
// short re-check while it cannot (no station IP yet), forever while
// disabled. After the station IP appears the first poll runs within
// VPOLL_OFFLINE_RECHECK_MS.
//
// The request uses esp_http_client_open / fetch_headers / read rather than
// perform() so the 4 KB body cap is enforced while reading. Every blocking
// socket operation is bounded by timeout_ms (VPOLL_TIMEOUT_MS); a server
// that accepts and never answers fails fetch_headers with EAGAIN after that
// time. A whole-request deadline on top bounds a server that trickles bytes.
// Redirects are followed by hand (the open/read API does not follow them).

#include "value_poll.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "http_auth.h"
#include "settings.h"
#include "value_feed.h"
#include "wifi_manager.h"

static const char *TAG = "vpoll";

#define BLOB_VPOLL               "vpoll"
#define VPOLL_TASK_PRIO          3
// TLS handshake (mbedtls) plus value_feed_ingest, which keeps a settings_t
// and the panel text on the stack. 6144 was the starting point; raised to
// 8192, the size the IDF HTTPS client examples use, since the handshake alone
// can approach 6 KB. To be confirmed on device: the task logs its stack
// high-water mark once after the first poll.
#define VPOLL_TASK_STACK         8192
#define VPOLL_OFFLINE_RECHECK_MS 5000
#define VPOLL_MAX_HOPS           5      // redirects + one auth retry
// Whole request (all hops, headers and body). Each blocking socket call is
// bounded by VPOLL_TIMEOUT_MS on its own; this caps the sum.
#define VPOLL_DEADLINE_MS        (2 * VPOLL_TIMEOUT_MS)

static SemaphoreHandle_t   s_lock;
static TaskHandle_t        s_task;
static value_poll_cfg_t    s_cfg = {.interval_s = VPOLL_INTERVAL_DEFAULT};
static value_poll_status_t s_status;

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static void cfg_defaults(value_poll_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->interval_s = VPOLL_INTERVAL_DEFAULT;
}

// ---- validation --------------------------------------------------------------

static bool url_scheme_ok(const char *url)
{
    return strncasecmp(url, "http://", 7) == 0 ||
           strncasecmp(url, "https://", 8) == 0;
}

const char *value_poll_validate(const value_poll_cfg_t *c)
{
    if (strnlen(c->url, sizeof(c->url)) > VPOLL_URL_MAX ||
        strnlen(c->hdr_name, sizeof(c->hdr_name)) > VPOLL_HDR_NAME_MAX ||
        strnlen(c->hdr_value, sizeof(c->hdr_value)) > VPOLL_SECRET_MAX ||
        strnlen(c->path, sizeof(c->path)) > JSONPATH_PATH_MAX) {
        return "field too long";
    }
    if (c->url[0] != '\0' && !url_scheme_ok(c->url)) {
        return "url must start with http:// or https://";
    }
    if (c->interval_s < VPOLL_INTERVAL_MIN || c->interval_s > VPOLL_INTERVAL_MAX) {
        return "interval_s must be 10..86400";
    }
    if (c->enabled && c->url[0] == '\0') {
        return "url required to enable polling";
    }
    if (c->hdr_name[0] != '\0' && !http_auth_name_valid(c->hdr_name)) {
        return "hdr_name must be a header token";
    }
    if (!http_auth_value_valid(c->hdr_value)) {
        return "hdr_value must not contain line breaks";
    }
    return NULL;
}

// ---- blob ----------------------------------------------------------------------

static void json_str(const cJSON *root, const char *key, char *dst, size_t sz)
{
    const cJSON *it = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(it)) snprintf(dst, sz, "%s", it->valuestring);
}

static void load_blob(value_poll_cfg_t *out)
{
    cfg_defaults(out);

    char *buf = malloc(SETTINGS_BLOB_MAX);
    if (!buf) return;
    if (settings_load_blob(BLOB_VPOLL, buf, SETTINGS_BLOB_MAX, NULL) != ESP_OK) {
        free(buf);
        return;
    }
    cJSON *root = cJSON_Parse(buf);
    memset(buf, 0, SETTINGS_BLOB_MAX);   // held the secrets
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "vpoll blob unparseable, using defaults");
        return;
    }
    out->enabled = cJSON_IsTrue(cJSON_GetObjectItem(root, "enabled"));
    json_str(root, "url",       out->url,       sizeof(out->url));
    json_str(root, "hdr_name",  out->hdr_name,  sizeof(out->hdr_name));
    json_str(root, "hdr_value", out->hdr_value, sizeof(out->hdr_value));
    json_str(root, "path",      out->path,      sizeof(out->path));
    const cJSON *iv = cJSON_GetObjectItem(root, "interval_s");
    if (cJSON_IsNumber(iv)) {
        double v = iv->valuedouble;
        out->interval_s = v < VPOLL_INTERVAL_MIN ? VPOLL_INTERVAL_MIN
                        : v > VPOLL_INTERVAL_MAX ? VPOLL_INTERVAL_MAX
                        : (uint32_t)v;
    }
    cJSON_Delete(root);

    // Lenient on load: a stored config that no longer validates is kept but
    // not polled, rather than dropped.
    if (out->url[0] != '\0' && !url_scheme_ok(out->url)) out->enabled = false;
    if (out->url[0] == '\0') out->enabled = false;
}

static esp_err_t save_blob(const value_poll_cfg_t *in)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddBoolToObject(root, "enabled", in->enabled);
    cJSON_AddStringToObject(root, "url", in->url);
    cJSON_AddNumberToObject(root, "interval_s", in->interval_s);
    cJSON_AddStringToObject(root, "hdr_name", in->hdr_name);
    cJSON_AddStringToObject(root, "hdr_value", in->hdr_value);
    cJSON_AddStringToObject(root, "path", in->path);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    esp_err_t err = settings_save_blob(BLOB_VPOLL, json);
    memset(json, 0, strlen(json));   // held the secrets
    cJSON_free(json);
    return err;
}

// ---- the request -----------------------------------------------------------------

static bool station_online(void)
{
    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);
    return w.mode == WIFI_MGR_MODE_STA && w.connected && w.ip[0] != '\0' &&
           strcmp(w.ip, "0.0.0.0") != 0;
}

static bool is_redirect(int code)
{
    return code == 301 || code == 302 || code == 303 || code == 307 || code == 308;
}

// Scheme and host of `url` (lower-case scheme, host without userinfo or
// port). Empty strings when the URL has no "://".
static void url_origin(const char *url, char *scheme, size_t ssz,
                       char *host, size_t hsz)
{
    scheme[0] = host[0] = '\0';
    const char *sep = strstr(url, "://");
    if (!sep) return;
    size_t n = (size_t)(sep - url);
    if (n >= ssz) n = ssz - 1;
    for (size_t i = 0; i < n; i++) {
        char c = url[i];
        scheme[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    scheme[n] = '\0';

    const char *a = sep + 3;
    const char *end = a + strcspn(a, "/?#");
    for (const char *p = a; p < end; p++) {
        if (*p == '@') a = p + 1;      // skip userinfo
    }
    const char *h_end;
    if (*a == '[') {                   // IPv6 literal: keep up to ']'
        const char *rb = memchr(a, ']', (size_t)(end - a));
        h_end = rb ? rb + 1 : end;
    } else {
        const char *colon = memchr(a, ':', (size_t)(end - a));
        h_end = colon ? colon : end;
    }
    n = (size_t)(h_end - a);
    if (n >= hsz) n = hsz - 1;
    memcpy(host, a, n);
    host[n] = '\0';
}

// True when a redirect to the client's current URL may keep the configured
// custom header: same host as the configured URL and no
// downgrade from https to http. An unreadable URL counts as a change.
static bool redirect_keeps_auth(esp_http_client_handle_t client,
                                const char *orig_url)
{
    char now[512];
    if (esp_http_client_get_url(client, now, sizeof(now)) != ESP_OK) return false;
    char s0[8], h0[128], s1[8], h1[128];
    url_origin(orig_url, s0, sizeof(s0), h0, sizeof(h0));
    url_origin(now, s1, sizeof(s1), h1, sizeof(h1));
    if (strcasecmp(h0, h1) != 0) return false;
    if (strcmp(s0, "https") == 0 && strcmp(s1, "https") != 0) return false;
    return true;
}

static bool deadline_passed(int64_t start_us)
{
    return esp_timer_get_time() - start_us > (int64_t)VPOLL_DEADLINE_MS * 1000;
}

// Describe a failed open in the status line. ESP_ERR_HTTP_CONNECT alone does
// not say whether DNS, TCP or TLS failed, so take the cause from esp-tls.
static void connect_error(esp_http_client_handle_t client, esp_err_t e,
                          char *err, size_t errsz)
{
    int tls_code = 0, tls_flags = 0;
    esp_err_t tls = esp_http_client_get_and_clear_last_tls_error(client, &tls_code,
                                                                &tls_flags);
    switch (tls) {
    case ESP_OK:
        snprintf(err, errsz, "%s", esp_err_to_name(e));
        break;
    case ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME:
        snprintf(err, errsz, "DNS lookup failed");
        break;
    case ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST:
    case ESP_ERR_ESP_TLS_CANNOT_CREATE_SOCKET:
        snprintf(err, errsz, "TCP connect failed");
        break;
    case ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT:
        snprintf(err, errsz, "TCP connect timed out");
        break;
    case ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT:
        snprintf(err, errsz, "TLS handshake timed out");
        break;
    case ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED:
        if (tls_flags) {
            snprintf(err, errsz, "TLS certificate not trusted (flags 0x%x)",
                     (unsigned)tls_flags);
        } else {
            snprintf(err, errsz, "TLS handshake failed (-0x%04x)",
                     (unsigned)-tls_code);
        }
        break;
    default:
        snprintf(err, errsz, "%s (-0x%04x)", esp_err_to_name(tls),
                 (unsigned)-tls_code);
        break;
    }
    ESP_LOGW(TAG, "connect failed: %s, esp-tls %s, code -0x%04x, flags 0x%x",
             esp_err_to_name(e), esp_err_to_name(tls), (unsigned)-tls_code,
             (unsigned)tls_flags);
}

// Fetch the body into a fresh heap buffer (NUL terminated, *out_body, caller
// frees). On failure returns false with `err` set and *out_body NULL.
// *http_status is the last status received (0 = none).
static bool fetch(const value_poll_cfg_t *cfg, char **out_body, int *http_status,
                  char *err, size_t errsz)
{
    *out_body    = NULL;
    *http_status = 0;
    int64_t start = esp_timer_get_time();

    esp_http_client_config_t hc = {
        .url               = cfg->url,
        .method            = HTTP_METHOD_GET,
        .timeout_ms        = VPOLL_TIMEOUT_MS,
        .buffer_size       = 2048,
        // Sized for a redirect target, not the configured URL (2.7.0 lesson).
        .buffer_size_tx    = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hc);
    if (!client) {
        snprintf(err, errsz, "invalid URL");
        return false;
    }
    http_auth_apply(client, cfg->hdr_name, cfg->hdr_value);

    bool ok = false;
    bool auth_tried = false;
    bool auth_dropped = false;
    char *body = NULL;
    int status = 0;
    int total = 0;

    for (int hop = 0;; hop++) {
        if (hop > VPOLL_MAX_HOPS || deadline_passed(start)) {
            snprintf(err, errsz, "%s", hop > VPOLL_MAX_HOPS
                                           ? esp_err_to_name(ESP_ERR_HTTP_MAX_REDIRECT)
                                           : esp_err_to_name(ESP_ERR_TIMEOUT));
            goto done;
        }
        esp_err_t e = esp_http_client_open(client, 0);
        if (e != ESP_OK) {
            connect_error(client, e, err, errsz);
            goto done;
        }
        int64_t clen = esp_http_client_fetch_headers(client);
        if (clen < 0) {
            snprintf(err, errsz, "%s",
                     clen == -ESP_ERR_HTTP_EAGAIN
                         ? esp_err_to_name(ESP_ERR_TIMEOUT)
                         : esp_err_to_name(ESP_ERR_HTTP_FETCH_HEADER));
            goto done;
        }
        status = esp_http_client_get_status_code(client);
        *http_status = status;

        // Redirect or a Basic/Digest challenge (credentials in the URL):
        // drop this connection rather than draining an unbounded body, then
        // reopen. esp_http_client_open reconnects after close.
        if (is_redirect(status) && esp_http_client_set_redirection(client) == ESP_OK) {
            esp_http_client_close(client);
            // A redirect to another host, or from https to plain http, must
            // not carry the configured credentials; once dropped they stay
            // off for every later hop.
            if (!auth_dropped && !redirect_keeps_auth(client, cfg->url)) {
                esp_http_client_delete_header(client, "Authorization");
                if (cfg->hdr_name[0]) {
                    esp_http_client_delete_header(client, cfg->hdr_name);
                }
                auth_dropped = true;
                ESP_LOGI(TAG, "redirect left the original host or https, auth headers dropped");
            }
            continue;
        }
        char *user = NULL;
        if (status == 401 && !auth_tried &&
            esp_http_client_get_username(client, &user) == ESP_OK && user) {
            auth_tried = true;
            if (esp_http_client_add_auth(client) == ESP_OK) {
                esp_http_client_close(client);
                continue;
            }
        }
        if (status < 200 || status >= 300) {
            snprintf(err, errsz, "HTTP %d", status);
            goto done;
        }
        if (clen > VPOLL_BODY_MAX) {
            snprintf(err, errsz, "response too large");
            goto done;
        }
        break;
    }

    // +1 to detect overflow by reading one byte past the cap, +1 for the NUL.
    body = malloc(VPOLL_BODY_MAX + 2);
    if (!body) {
        snprintf(err, errsz, "%s", esp_err_to_name(ESP_ERR_NO_MEM));
        goto done;
    }
    for (;;) {
        if (deadline_passed(start)) {
            snprintf(err, errsz, "%s", esp_err_to_name(ESP_ERR_TIMEOUT));
            goto done;
        }
        int room = VPOLL_BODY_MAX + 1 - total;
        int n = esp_http_client_read(client, body + total, room);
        if (n == -ESP_ERR_HTTP_EAGAIN) {
            snprintf(err, errsz, "%s", esp_err_to_name(ESP_ERR_TIMEOUT));
            goto done;
        }
        if (n < 0) {
            snprintf(err, errsz, "%s", esp_err_to_name(ESP_FAIL));
            goto done;
        }
        if (n == 0) break;
        total += n;
        if (total > VPOLL_BODY_MAX) {
            snprintf(err, errsz, "response too large");
            goto done;
        }
    }
    body[total] = '\0';
    ok = true;

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (ok) {
        *out_body = body;
    } else {
        free(body);
    }
    return ok;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
                     s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
    return s;
}

static void poll_once(const value_poll_cfg_t *cfg)
{
    char err[sizeof(s_status.error)] = "";
    int  http = 0;
    char *body = NULL;
    char *extracted = NULL;   // heap, only for a JSON path
    const char *value = NULL;

    if (fetch(cfg, &body, &http, err, sizeof(err))) {
        char *t = trim(body);
        if (cfg->path[0] == '\0') {
            if (t[0] == '\0') {
                snprintf(err, sizeof(err), "empty response");
            } else {
                value = t;
            }
        } else if (t[0] == '\0') {
            snprintf(err, sizeof(err), "empty response");
        } else {
            extracted = malloc(VPOLL_BODY_MAX + 1);
            if (!extracted) {
                snprintf(err, sizeof(err), "%s", esp_err_to_name(ESP_ERR_NO_MEM));
            } else {
                jsonpath_err_t je = jsonpath_extract(t, cfg->path, extracted,
                                                     VPOLL_BODY_MAX + 1);
                if (je != JSONPATH_OK) {
                    snprintf(err, sizeof(err), "%s", jsonpath_err_str(je));
                } else if (extracted[0] == '\0') {
                    snprintf(err, sizeof(err), "empty response");
                } else {
                    value = extracted;
                }
            }
        }
    }

    lock();
    s_status.last_poll   = (int64_t)time(NULL);
    s_status.http_status = http;
    if (value) {
        snprintf(s_status.raw, sizeof(s_status.raw), "%s", value);
        s_status.error[0] = '\0';
        s_status.failures = 0;
    } else {
        s_status.raw[0] = '\0';
        snprintf(s_status.error, sizeof(s_status.error), "%s", err);
        if (s_status.failures < UINT16_MAX) s_status.failures++;
    }
    unlock();

    if (value) {
        value_feed_ingest(value, VALUE_SRC_POLL, NULL, 0);
    } else {
        // Status codes and error names only: never the URL or the body.
        ESP_LOGW(TAG, "poll failed (HTTP %d): %s", http, err);
        value_feed_poll_failed();
    }
    free(extracted);
    free(body);
}

// ---- task ------------------------------------------------------------------------

static TickType_t ms_to_ticks(int64_t ms)
{
    if (ms <= 0) return 0;
    uint64_t t = (uint64_t)ms * configTICK_RATE_HZ / 1000;
    return t >= portMAX_DELAY ? portMAX_DELAY - 1 : (TickType_t)t;
}

static void poll_task(void *arg)
{
    (void)arg;
    int64_t next_due_us = 0;   // 0 = poll as soon as polling can run
    bool hwm_logged = false;

    for (;;) {
        value_poll_cfg_t cfg;
        value_poll_get(&cfg);
        bool active = cfg.enabled && cfg.url[0] != '\0';
        bool online = station_online();

        if (active && online && esp_timer_get_time() >= next_due_us) {
            poll_once(&cfg);
            next_due_us = esp_timer_get_time() + (int64_t)cfg.interval_s * 1000000;
            if (!hwm_logged) {
                hwm_logged = true;
                ESP_LOGI(TAG, "stack high-water mark after first poll: %u bytes free",
                         (unsigned)uxTaskGetStackHighWaterMark(NULL));
            }
        }
        // Offline: poll shortly after the station IP comes (back).
        if (!online) next_due_us = 0;

        TickType_t wait;
        if (!active) {
            wait = portMAX_DELAY;
        } else if (!online) {
            wait = ms_to_ticks(VPOLL_OFFLINE_RECHECK_MS);
        } else {
            wait = ms_to_ticks((next_due_us - esp_timer_get_time()) / 1000);
        }
        memset(&cfg, 0, sizeof(cfg));   // held the secrets

        if (ulTaskNotifyTake(pdTRUE, wait) > 0) {
            next_due_us = 0;   // saved config or "poll now": poll at once
        }
    }
}

// ---- public API ----------------------------------------------------------------

esp_err_t value_poll_init(void)
{
    if (s_task) return ESP_OK;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    value_poll_cfg_t cfg;
    load_blob(&cfg);
    lock();
    s_cfg = cfg;
    memset(&s_status, 0, sizeof(s_status));
    unlock();
    memset(&cfg, 0, sizeof(cfg));

    if (xTaskCreate(poll_task, "vpoll", VPOLL_TASK_STACK, NULL, VPOLL_TASK_PRIO,
                    &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "poller started");
    return ESP_OK;
}

void value_poll_get(value_poll_cfg_t *out)
{
    lock();
    *out = s_cfg;
    unlock();
}

esp_err_t value_poll_set(const value_poll_cfg_t *in)
{
    if (value_poll_validate(in) != NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t err = save_blob(in);
    if (err != ESP_OK) return err;
    lock();
    s_cfg = *in;
    unlock();
    value_poll_now();
    return ESP_OK;
}

void value_poll_get_status(value_poll_status_t *out)
{
    lock();
    *out = s_status;
    unlock();
}

void value_poll_now(void)
{
    if (s_task) xTaskNotifyGive(s_task);
}

bool value_poll_ready(void)
{
    value_poll_cfg_t cfg;
    value_poll_get(&cfg);
    bool ready = cfg.enabled && cfg.url[0] != '\0' && station_online();
    memset(&cfg, 0, sizeof(cfg));
    return ready;
}
