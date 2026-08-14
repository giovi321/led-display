#include "web_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "integrations.h"
#include "wifi_manager.h"
#include "wg_client.h"

// ---- responses -----------------------------------------------------------

esp_err_t web_send_json(httpd_req_t *req, cJSON *root)
{
    char *out = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, out ? out : "{}");
    if (out) cJSON_free(out);
    cJSON_Delete(root);
    return err;
}

esp_err_t web_send_ok(httpd_req_t *req)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    return web_send_json(req, r);
}

esp_err_t web_send_err(httpd_req_t *req, const char *msg, int code)
{
    char status[32];
    snprintf(status, sizeof(status), "%d Error", code);
    httpd_resp_set_status(req, status);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", false);
    cJSON_AddStringToObject(r, "error", msg);
    return web_send_json(req, r);
}

// ---- requests ------------------------------------------------------------

char *web_read_body(httpd_req_t *req, size_t max_len)
{
    int total = req->content_len;
    if (total <= 0 || (size_t)total > max_len) return NULL;
    char *buf = malloc(total + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, total - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[total] = '\0';
    return buf;
}

cJSON *web_read_json(httpd_req_t *req, size_t max_len)
{
    char *body = web_read_body(req, max_len);
    if (!body) return NULL;
    cJSON *json = cJSON_Parse(body);
    free(body);
    return json;
}

// ---- auth ----------------------------------------------------------------

// Constant-time comparison so the key can't be guessed byte by byte from
// response timing. Length differences still change timing; acceptable for
// the LAN threat model documented in DESIGN.md.
static bool key_equal(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (unsigned char)(la ^ lb);
    size_t n = la < lb ? la : lb;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

// ---- cross-site request hardening -----------------------------------------
//
// The API key is optional, so in the documented LAN-trust default any web
// page the user's browser loads could POST to the device (CSRF) or, with DNS
// rebinding, even read responses. Two cheap checks close that off: the Host
// header must be an address this device actually answers to, and a browser
// Origin header, when present, must point at the device. Skipped in AP mode:
// the captive-portal sheet posts /api/wifi/connect under the probe hostname
// it was hijacked from.

static bool host_is_us(const char *host, const char *ip)
{
    size_t n = strlen(ip);
    if (n == 0) return false;
    return strncmp(host, ip, n) == 0 && (host[n] == '\0' || host[n] == ':');
}

static bool request_origin_ok(httpd_req_t *req)
{
    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);
    if (w.mode != WIFI_MGR_MODE_STA) {
        return true;
    }

    // Addresses this device legitimately answers on: the WiFi IP and, when
    // the tunnel is configured, the WireGuard address (without its /mask).
    char wg_ip[WG_ADDR_MAX + 1] = "";
    wg_config_t wg;
    wg_client_get_config(&wg);
    if (wg.address[0] != '\0') {
        snprintf(wg_ip, sizeof(wg_ip), "%s", wg.address);
        char *slash = strchr(wg_ip, '/');
        if (slash) *slash = '\0';
    }

    char hdr[96];
    if (httpd_req_get_hdr_value_str(req, "Host", hdr, sizeof(hdr)) == ESP_OK) {
        if (!host_is_us(hdr, w.ip) && !host_is_us(hdr, wg_ip)) {
            return false;   // DNS rebinding or a request meant for elsewhere
        }
    }
    if (httpd_req_get_hdr_value_str(req, "Origin", hdr, sizeof(hdr)) == ESP_OK) {
        const char *h = strncmp(hdr, "http://", 7) == 0 ? hdr + 7 : hdr;
        if (!host_is_us(h, w.ip) && !host_is_us(h, wg_ip)) {
            return false;   // cross-site browser request (CSRF)
        }
    }
    return true;
}

bool web_auth_ok(httpd_req_t *req)
{
    integrations_cfg_t cfg;
    integrations_get(&cfg);
    if (cfg.api_key[0] == '\0') {
        return true;   // no key configured: open (LAN-trust default)
    }

    char hdr[INTEGR_API_KEY_MAX + 1] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Api-Key", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    return key_equal(hdr, cfg.api_key);
}

esp_err_t web_require_auth(httpd_req_t *req)
{
    if (!request_origin_ok(req)) {
        web_send_err(req, "cross-origin request rejected", 403);
        return ESP_FAIL;
    }
    if (web_auth_ok(req)) {
        return ESP_OK;
    }
    web_send_err(req, "missing or wrong X-Api-Key", 401);
    return ESP_FAIL;
}
