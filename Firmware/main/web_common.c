#include "web_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "mbedtls/base64.h"

#include "devname.h"
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
// header must be a name or address this device actually answers to - its WiFi
// IP, its WireGuard address, or its own device name, which is what a router
// registers from the DHCP hostname - and a browser Origin header, when present,
// must point at the same. Skipped in AP mode: the captive-portal sheet posts
// /api/wifi/connect under the probe hostname it was hijacked from.

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

    // The resolved device name, which is also the DHCP hostname: browsing to
    // http://<name>/ is the documented way in, so its writes must not 403.
    char name[INTEGR_NAME_MAX + 1];
    integrations_get_device_name(name, sizeof(name));

    char hdr[96];
    if (httpd_req_get_hdr_value_str(req, "Host", hdr, sizeof(hdr)) == ESP_OK) {
        if (!host_is_us(hdr, w.ip) && !host_is_us(hdr, wg_ip) &&
            !devname_host_matches(hdr, name)) {
            return false;   // DNS rebinding or a request meant for elsewhere
        }
    }
    if (httpd_req_get_hdr_value_str(req, "Origin", hdr, sizeof(hdr)) == ESP_OK) {
        const char *h = strncmp(hdr, "http://", 7) == 0 ? hdr + 7 : hdr;
        if (!host_is_us(h, w.ip) && !host_is_us(h, wg_ip) &&
            !devname_host_matches(h, name)) {
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

// ---- browser login (HTTP Basic) --------------------------------------------

#define BASIC_PREFIX     "Basic "
#define BASIC_PREFIX_LEN (sizeof(BASIC_PREFIX) - 1)
// "user:pass" at the configured maxima, base64'd (4 bytes out per 3 in),
// plus the scheme prefix and room for the NUL and any padding.
#define AUTH_HDR_MAX     (BASIC_PREFIX_LEN + \
                          (((INTEGR_UI_USER_MAX + 1 + INTEGR_UI_PASS_MAX) + 2) / 3) * 4 + 8)

// Whether the browser login is on AND applies to this request. AP mode is
// exempt for the reasons in web_common.h.
static bool login_applies(const integrations_cfg_t *cfg)
{
    if (!cfg->ui_auth || cfg->ui_pass[0] == '\0') return false;

    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);
    return w.mode == WIFI_MGR_MODE_STA;
}

static bool basic_credentials_ok(httpd_req_t *req, const integrations_cfg_t *cfg)
{
    char hdr[AUTH_HDR_MAX] = {0};
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    if (strncmp(hdr, BASIC_PREFIX, BASIC_PREFIX_LEN) != 0) {
        return false;
    }

    unsigned char decoded[INTEGR_UI_USER_MAX + 1 + INTEGR_UI_PASS_MAX + 1] = {0};
    size_t out_len = 0;
    const unsigned char *b64 = (const unsigned char *)(hdr + BASIC_PREFIX_LEN);
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &out_len,
                              b64, strlen((const char *)b64)) != 0) {
        return false;   // not base64, or longer than any credential we accept
    }
    decoded[out_len] = '\0';

    // "user:password"; the password may itself contain colons, the username
    // may not, so split on the FIRST one.
    char *sep = strchr((char *)decoded, ':');
    if (!sep) return false;
    *sep = '\0';

    // Both halves compared in constant time, and both always compared, so a
    // wrong username costs the same as a wrong password.
    bool user_ok = key_equal((const char *)decoded, cfg->ui_user);
    bool pass_ok = key_equal(sep + 1, cfg->ui_pass);
    return user_ok && pass_ok;
}

// 401 with the challenge that makes a browser show its login sheet.
static esp_err_t send_login_challenge(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate",
                       "Basic realm=\"led-display\", charset=\"UTF-8\"");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, "Authentication required\n");
    // ESP_FAIL closes the session, which matters because the gate runs before
    // the handler reads anything: a rejected POST still has its whole body
    // sitting in the socket, and keeping the connection alive would have
    // httpd parse those bytes as the next request.
    return ESP_FAIL;
}

// One heap record per registered URI, holding what the trampoline needs to
// call the real handler. Never freed: the server's URI table lives as long as
// the server does.
typedef struct {
    esp_err_t (*handler)(httpd_req_t *);
    void      *user_ctx;
} gated_uri_t;

static esp_err_t login_gate(httpd_req_t *req)
{
    const gated_uri_t *g = (const gated_uri_t *)req->user_ctx;

    integrations_cfg_t cfg;
    integrations_get(&cfg);
    if (login_applies(&cfg) && !basic_credentials_ok(req, &cfg)) {
        return send_login_challenge(req);
    }

    // Hand the real handler the user_ctx it was registered with, so anything
    // that reads req->user_ctx (the static asset table) still works.
    req->user_ctx = g->user_ctx;
    return g->handler(req);
}

esp_err_t web_register_uri(httpd_handle_t srv, const httpd_uri_t *uri)
{
    gated_uri_t *g = malloc(sizeof(*g));
    if (!g) return ESP_ERR_NO_MEM;
    g->handler  = uri->handler;
    g->user_ctx = uri->user_ctx;

    httpd_uri_t gated = *uri;
    gated.handler  = login_gate;
    gated.user_ctx = g;

    esp_err_t err = httpd_register_uri_handler(srv, &gated);
    if (err != ESP_OK) free(g);
    return err;
}
