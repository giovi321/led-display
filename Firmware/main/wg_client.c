// WireGuard client glue. The real implementation (behind
// CONFIG_LEDDISP_WIREGUARD) drives the vendored esp_wireguard component; the
// no-op branch below stays as the permanent fallback so callers never need
// #ifdefs.
//
// Blob shape (versioned for forward migration, DESIGN.md section 4):
//   "wg" {"v":1,"enabled":false,"private_key":"..","peer_public_key":"..",
//         "endpoint_host":"..","endpoint_port":51820,"address":"10.6.0.2/32",
//         "allowed_ips":"10.6.0.0/24","keepalive_s":25}

#include "wg_client.h"

#include <string.h>

#include "sdkconfig.h"

#if CONFIG_LEDDISP_WIREGUARD

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_wireguard.h"
#include "settings.h"

static const char *TAG = "wg";

#define BLOB_WG            "wg"
#define WG_TASK_STACK      4096
#define WG_TASK_PRIO       3            // below the display refresh, above idle
#define WG_BACKOFF_MIN_MS  5000
#define WG_BACKOFF_MAX_MS  60000
// Handshake timestamps (tai64n) need real wall time or the peer rejects the
// initiation as a replay. 2020-01-01 as "clock looks synced" heuristic.
#define WG_TIME_SANE       1577836800
#define WG_TIME_WAIT_S     90
// No completed handshake for this long after connect -> tear down and redo
// init, which also re-resolves the endpoint hostname (DNS may have changed).
#define WG_STALL_S         180

static SemaphoreHandle_t s_lock;
static wg_config_t       s_cfg;                 // persisted config (RAM cache)
static TaskHandle_t      s_task;
static volatile bool     s_task_stop;

// The component keeps the const char* pointers we hand it for the lifetime of
// the tunnel, so they must point at storage that survives until teardown:
// a dedicated "active" copy, only rewritten while the tunnel is down.
static wg_config_t       s_active;
static char              s_active_ip[WG_ADDR_MAX + 1];   // address, CIDR stripped
static char              s_active_mask[16];              // dotted netmask
static wireguard_config_t s_wg_cfg;
static wireguard_ctx_t    s_wg_ctx;
static bool               s_tunnel_up_attempted;   // netif exists (init+connect issued)

// Tolerate pre-init callers, same pattern as integrations.c.
static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// ---- config (de)serialisation -----------------------------------------------

static void cfg_defaults(wg_config_t *c)
{
    memset(c, 0, sizeof(*c));
    c->endpoint_port = 51820;
    c->keepalive_s   = 25;
}

static void json_get_str(const cJSON *root, const char *key,
                         char *dst, size_t dstsz)
{
    const cJSON *it = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(it)) snprintf(dst, dstsz, "%s", it->valuestring);
}

static void load_cfg(wg_config_t *out)
{
    cfg_defaults(out);

    char buf[SETTINGS_BLOB_MAX];
    if (settings_load_blob(BLOB_WG, buf, sizeof(buf), NULL) != ESP_OK) {
        return;   // never written: defaults (disabled)
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "wg blob unparseable, using defaults");
        return;
    }

    const cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(en)) out->enabled = cJSON_IsTrue(en);

    json_get_str(root, "private_key",     out->private_key,     sizeof(out->private_key));
    json_get_str(root, "peer_public_key", out->peer_public_key, sizeof(out->peer_public_key));
    json_get_str(root, "endpoint_host",   out->endpoint_host,   sizeof(out->endpoint_host));
    json_get_str(root, "address",         out->address,         sizeof(out->address));
    json_get_str(root, "allowed_ips",     out->allowed_ips,     sizeof(out->allowed_ips));

    const cJSON *port = cJSON_GetObjectItem(root, "endpoint_port");
    if (cJSON_IsNumber(port) && port->valueint > 0 && port->valueint <= 0xFFFF) {
        out->endpoint_port = (uint16_t)port->valueint;
    }
    const cJSON *ka = cJSON_GetObjectItem(root, "keepalive_s");
    if (cJSON_IsNumber(ka) && ka->valueint >= 0 && ka->valueint <= 0xFFFF) {
        out->keepalive_s = (uint16_t)ka->valueint;
    }

    cJSON_Delete(root);
}

static esp_err_t save_cfg(const wg_config_t *in)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddBoolToObject(root, "enabled", in->enabled);
    cJSON_AddStringToObject(root, "private_key", in->private_key);
    cJSON_AddStringToObject(root, "peer_public_key", in->peer_public_key);
    cJSON_AddStringToObject(root, "endpoint_host", in->endpoint_host);
    cJSON_AddNumberToObject(root, "endpoint_port", in->endpoint_port);
    cJSON_AddStringToObject(root, "address", in->address);
    cJSON_AddStringToObject(root, "allowed_ips", in->allowed_ips);
    cJSON_AddNumberToObject(root, "keepalive_s", in->keepalive_s);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    esp_err_t err = settings_save_blob(BLOB_WG, json);
    cJSON_free(json);
    return err;
}

// ---- CIDR helpers ------------------------------------------------------------

// "10.6.0.12/32" -> ip "10.6.0.12", prefix 32. No suffix means /32: safest
// default, routes only the tunnel address itself.
static void split_cidr(const char *in, char *ip, size_t ipsz, int *prefix)
{
    *prefix = 32;
    snprintf(ip, ipsz, "%s", in);
    char *slash = strchr(ip, '/');
    if (slash) {
        *slash = '\0';
        int p = atoi(slash + 1);
        if (p >= 0 && p <= 32) *prefix = p;
    }
}

static void prefix_to_netmask(int prefix, char *out, size_t outsz)
{
    uint32_t mask = (prefix == 0) ? 0 : 0xFFFFFFFFu << (32 - prefix);
    snprintf(out, outsz, "%u.%u.%u.%u",
             (unsigned)(mask >> 24) & 0xFF, (unsigned)(mask >> 16) & 0xFF,
             (unsigned)(mask >> 8) & 0xFF, (unsigned)mask & 0xFF);
}

// ---- component calls, marshalled into the lwIP thread ------------------------
//
// esp_wireguard drives raw lwIP APIs (netif_add, dns_gethostbyname, udp_*)
// which are only thread-safe from the tcpip thread unless
// CONFIG_LWIP_TCPIP_CORE_LOCKING is on (it is off by default), so every
// component call goes through esp_netif_tcpip_exec().

static esp_err_t tcpip_wg_init(void *arg)
{
    (void)arg;
    return esp_wireguard_init(&s_wg_cfg, &s_wg_ctx);
}

static esp_err_t tcpip_wg_connect(void *arg)
{
    (void)arg;
    return esp_wireguard_connect(&s_wg_ctx);
}

static esp_err_t tcpip_wg_disconnect(void *arg)
{
    (void)arg;
    return esp_wireguard_disconnect(&s_wg_ctx);
}

static esp_err_t tcpip_wg_peer_up(void *arg)
{
    (void)arg;
    return esp_wireguard_peer_is_up(&s_wg_ctx);
}

static esp_err_t tcpip_wg_handshake(void *arg)
{
    return esp_wireguard_latest_handshake(&s_wg_ctx, (time_t *)arg);
}

typedef struct {
    const char *ip;
    const char *mask;
} allowed_ip_arg_t;

static esp_err_t tcpip_wg_allow(void *arg)
{
    const allowed_ip_arg_t *a = (const allowed_ip_arg_t *)arg;
    return esp_wireguard_add_allowed_ip(&s_wg_ctx, a->ip, a->mask);
}

static esp_err_t tcpip_wg_set_default(void *arg)
{
    (void)arg;
    return esp_wireguard_set_default(&s_wg_ctx);
}

// ---- tunnel lifecycle (caller holds s_lock) -----------------------------------

static void tunnel_teardown(void)
{
    if (!s_tunnel_up_attempted) return;
    // esp_wireguard_disconnect removes the peer and netif and restores the
    // previous default netif, giving us a clean slate for the next init.
    esp_err_t err = esp_netif_tcpip_exec(tcpip_wg_disconnect, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "teardown: %s", esp_err_to_name(err));
    }
    s_tunnel_up_attempted = false;
}

// Fill the component config from s_active. Strings passed to the component
// must stay valid for the tunnel lifetime, hence the static buffers.
static esp_err_t tunnel_start(void)
{
    int prefix;
    split_cidr(s_active.address, s_active_ip, sizeof(s_active_ip), &prefix);
    prefix_to_netmask(prefix, s_active_mask, sizeof(s_active_mask));

    wireguard_config_t cfg = ESP_WIREGUARD_CONFIG_DEFAULT();
    cfg.private_key          = s_active.private_key;
    cfg.public_key           = s_active.peer_public_key;
    cfg.address              = s_active_ip;
    cfg.netmask              = s_active_mask;
    cfg.endpoint             = s_active.endpoint_host;
    cfg.port                 = s_active.endpoint_port;
    cfg.persistent_keepalive = s_active.keepalive_s;
    s_wg_cfg = cfg;

    esp_err_t err = esp_netif_tcpip_exec(tcpip_wg_init, NULL);
    if (err != ESP_OK) return err;

    err = esp_netif_tcpip_exec(tcpip_wg_connect, NULL);
    // Once init/connect ran, a netif may exist even on failure; remember so
    // the next attempt starts from a teardown.
    s_tunnel_up_attempted = true;
    return err;
}

// Route the configured allowed_ips (comma-separated CIDRs) through the
// tunnel. "0.0.0.0/0" additionally makes WireGuard the default interface.
static void tunnel_apply_allowed_ips(void)
{
    char list[WG_ALLOWED_MAX + 1];
    snprintf(list, sizeof(list), "%s", s_active.allowed_ips);

    char *save = NULL;
    for (char *tok = strtok_r(list, ", ", &save); tok;
         tok = strtok_r(NULL, ", ", &save)) {
        char ip[WG_ADDR_MAX + 1];
        char mask[16];
        int prefix;
        split_cidr(tok, ip, sizeof(ip), &prefix);
        prefix_to_netmask(prefix, mask, sizeof(mask));

        allowed_ip_arg_t arg = {.ip = ip, .mask = mask};
        esp_err_t err = esp_netif_tcpip_exec(tcpip_wg_allow, &arg);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "allowed_ip %s/%d: %s", ip, prefix,
                     esp_err_to_name(err));
        }
        if (prefix == 0) {
            // Full-tunnel config: everything should leave through WireGuard.
            (void)esp_netif_tcpip_exec(tcpip_wg_set_default, NULL);
        }
    }
}

// ---- retry/monitor task --------------------------------------------------------

// Sleep in short slices so wg_client_apply() can stop the task promptly even
// mid-backoff.
static bool task_sleep(uint32_t ms)
{
    while (ms > 0 && !s_task_stop) {
        uint32_t slice = ms > 250 ? 250 : ms;
        vTaskDelay(pdMS_TO_TICKS(slice));
        ms -= slice;
    }
    return !s_task_stop;
}

static void wg_task(void *arg)
{
    (void)arg;
    uint32_t backoff_ms = WG_BACKOFF_MIN_MS;

    // Handshake timestamps need wall time (SNTP runs elsewhere in boot).
    // Proceed after a grace period anyway: a peer that has never seen this
    // device accepts any initial timestamp.
    int waited = 0;
    while (!s_task_stop && time(NULL) < WG_TIME_SANE && waited < WG_TIME_WAIT_S) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        waited += 2;
    }
    if (!s_task_stop && time(NULL) < WG_TIME_SANE) {
        ESP_LOGW(TAG, "time not synced after %ds, trying anyway", waited);
    }

    while (!s_task_stop) {
        lock();
        tunnel_teardown();
        esp_err_t err = tunnel_start();
        unlock();

        if (err != ESP_OK) {
            // Covers WiFi not up yet, DNS still resolving (ESP_ERR_RETRY)
            // and unreachable endpoints; all retried with backoff.
            ESP_LOGI(TAG, "connect: %s, retry in %us",
                     esp_err_to_name(err), (unsigned)(backoff_ms / 1000));
            if (!task_sleep(backoff_ms)) break;
            backoff_ms = backoff_ms * 2 > WG_BACKOFF_MAX_MS ? WG_BACKOFF_MAX_MS
                                                            : backoff_ms * 2;
            continue;
        }

        lock();
        tunnel_apply_allowed_ips();
        unlock();
        backoff_ms = WG_BACKOFF_MIN_MS;

        // Monitor: the component's own timers handle rekeying and handshake
        // retries; we only watch for a long stall (endpoint moved, WiFi died)
        // and then rebuild the tunnel, which re-resolves the hostname.
        int down_s = 0;
        bool was_up = false;
        while (!s_task_stop) {
            if (!task_sleep(10000)) break;
            lock();
            bool up = esp_netif_tcpip_exec(tcpip_wg_peer_up, NULL) == ESP_OK;
            unlock();
            if (up) {
                if (!was_up) ESP_LOGI(TAG, "peer up");
                was_up = true;
                down_s = 0;
            } else {
                down_s += 10;
                if (down_s >= WG_STALL_S) {
                    ESP_LOGW(TAG, "no handshake for %ds, rebuilding tunnel",
                             down_s);
                    break;   // outer loop tears down and reconnects
                }
            }
        }
    }

    lock();
    tunnel_teardown();
    unlock();
    s_task = NULL;
    vTaskDelete(NULL);
}

// Caller must NOT hold s_lock (the task takes it on its way out).
static void task_stop(void)
{
    s_task_stop = true;
    // The task polls the flag at least every 250 ms; 5 s covers a slow
    // teardown before we give up and log.
    for (int i = 0; i < 500 && s_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_task) {
        ESP_LOGE(TAG, "task did not stop, tunnel state may be inconsistent");
    }
    s_task_stop = false;
}

static esp_err_t task_start(void)
{
    if (s_task) return ESP_OK;
    s_task_stop = false;
    if (xTaskCreate(wg_task, "wg", WG_TASK_STACK, NULL, WG_TASK_PRIO,
                    &s_task) != pdPASS) {
        s_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

// ---- public API ----------------------------------------------------------------

esp_err_t wg_client_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    wg_config_t cfg;
    load_cfg(&cfg);

    lock();
    s_cfg    = cfg;
    s_active = cfg;
    unlock();

    // Keys never appear in logs (DESIGN.md section 10); the endpoint host is
    // fine, the component logs it during connect anyway.
    ESP_LOGI(TAG, "init: enabled=%d endpoint=%s:%u keepalive=%us keys=%s",
             cfg.enabled, cfg.endpoint_host[0] ? cfg.endpoint_host : "(unset)",
             (unsigned)cfg.endpoint_port, (unsigned)cfg.keepalive_s,
             cfg.private_key[0] ? "[redacted]" : "(unset)");

    if (cfg.enabled) {
        return task_start();   // connects in the background, never blocks boot
    }
    return ESP_OK;
}

esp_err_t wg_client_apply(const wg_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    // Defensive copy that guarantees NUL termination whatever the HTTP
    // handler stuffed into the fixed arrays.
    wg_config_t c = *cfg;
    c.private_key[WG_KEY_MAX]       = '\0';
    c.peer_public_key[WG_KEY_MAX]   = '\0';
    c.endpoint_host[WG_HOST_MAX]    = '\0';
    c.address[WG_ADDR_MAX]          = '\0';
    c.allowed_ips[WG_ALLOWED_MAX]   = '\0';

    if (c.enabled) {
        if (!c.private_key[0] || !c.peer_public_key[0] ||
            !c.endpoint_host[0] || !c.address[0] || c.endpoint_port == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    esp_err_t err = save_cfg(&c);
    if (err != ESP_OK) return err;

    // Stop the worker before swapping the active config: the component holds
    // pointers into s_active while the tunnel exists.
    task_stop();

    lock();
    tunnel_teardown();
    s_cfg    = c;
    s_active = c;
    unlock();

    ESP_LOGI(TAG, "apply: enabled=%d endpoint=%s:%u", c.enabled,
             c.endpoint_host[0] ? c.endpoint_host : "(unset)",
             (unsigned)c.endpoint_port);

    if (c.enabled) {
        return task_start();
    }
    return ESP_OK;
}

void wg_client_get_config(wg_config_t *out)
{
    if (!out) return;
    lock();
    *out = s_cfg;   // includes the private key; the export endpoint is gated
    unlock();
}

void wg_client_get_status(wg_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->compiled_in = true;

    lock();
    out->enabled = s_cfg.enabled;
    if (s_tunnel_up_attempted) {
        out->up = esp_netif_tcpip_exec(tcpip_wg_peer_up, NULL) == ESP_OK;
        time_t hs = 0;
        if (esp_netif_tcpip_exec(tcpip_wg_handshake, &hs) == ESP_OK) {
            out->last_handshake = (int64_t)hs;
        }
    }
    unlock();
}

#else  // !CONFIG_LEDDISP_WIREGUARD

esp_err_t wg_client_init(void)
{
    return ESP_OK;
}

esp_err_t wg_client_apply(const wg_config_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

void wg_client_get_config(wg_config_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

void wg_client_get_status(wg_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->compiled_in = false;
}

#endif  // CONFIG_LEDDISP_WIREGUARD
