// /api/integrations and /api/wireguard (see api_handlers.h for the
// ownership table, DESIGN.md section 5 for shapes).
//
// Secrets policy (DESIGN.md section 10): the GETs here stay open, so they
// must never echo the API key or the WireGuard private key back - they
// report *_set booleans instead. The full secrets only leave the device via
// the key-protected /api/config/export.

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "api_handlers.h"
#include "integrations.h"
#include "mqtt_link.h"
#include "web_common.h"
#include "wg_client.h"

static const char *TAG = "api_integr";

// Webhook events travel through the JSON API as name strings so a config
// file stays readable; the bitmask is an implementation detail.
typedef struct {
    const char *name;
    uint32_t    bit;
} event_map_t;

static const event_map_t EVENTS[] = {
    {"boot",         WEBHOOK_EVT_BOOT},
    {"ip_change",    WEBHOOK_EVT_IP_CHANGE},
    {"ota_result",   WEBHOOK_EVT_OTA_RESULT},
    {"schedule_on",  WEBHOOK_EVT_SCHEDULE_ON},
    {"schedule_off", WEBHOOK_EVT_SCHEDULE_OFF},
    {"value_stale",  WEBHOOK_EVT_VALUE_STALE},
    {"value_recovered", WEBHOOK_EVT_VALUE_RECOVERED},
    // "status" is deliberately absent: the heartbeat (heartbeat_min) sends
    // it, so it is not selectable here.
};
#define EVENT_COUNT (sizeof(EVENTS) / sizeof(EVENTS[0]))

static cJSON *events_to_json(uint32_t mask)
{
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < EVENT_COUNT; i++) {
        if (mask & EVENTS[i].bit) {
            cJSON_AddItemToArray(arr, cJSON_CreateString(EVENTS[i].name));
        }
    }
    return arr;
}

// Parse ["boot","value_stale",...] into a mask. Returns false on anything
// that is not an array of known names (typos should 400, not vanish).
static bool events_from_json(const cJSON *arr, uint32_t *out_mask)
{
    if (!cJSON_IsArray(arr)) return false;
    uint32_t mask = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it)) return false;
        bool found = false;
        for (size_t i = 0; i < EVENT_COUNT; i++) {
            if (strcmp(it->valuestring, EVENTS[i].name) == 0) {
                mask |= EVENTS[i].bit;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    *out_mask = mask;
    return true;
}

// ---- /api/integrations -----------------------------------------------------

static esp_err_t integrations_get_handler(httpd_req_t *req)
{
    integrations_cfg_t cfg;
    integrations_get(&cfg);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "api_key_set", cfg.api_key[0] != '\0');
    cJSON_AddStringToObject(r, "webhook_url", cfg.webhook_url);
    cJSON_AddItemToObject(r, "webhook_events", events_to_json(cfg.webhook_events));
    cJSON_AddStringToObject(r, "device_name", cfg.device_name);
    // The username is not a secret and the UI needs it to prefill the field;
    // the password only ever reports whether one is set, like api_key above.
    cJSON_AddBoolToObject(r, "ui_auth", cfg.ui_auth);
    cJSON_AddStringToObject(r, "ui_user", cfg.ui_user);
    cJSON_AddBoolToObject(r, "ui_pass_set", cfg.ui_pass[0] != '\0');
    // Outbound webhook auth: the header name is not a secret, the secret and
    // the header value only report whether one is set.
    cJSON_AddBoolToObject(r, "webhook_secret_set", cfg.webhook_secret[0] != '\0');
    cJSON_AddStringToObject(r, "webhook_hdr_name", cfg.webhook_hdr_name);
    cJSON_AddBoolToObject(r, "webhook_hdr_value_set",
                          cfg.webhook_hdr_value[0] != '\0');
    cJSON_AddNumberToObject(r, "heartbeat_min", cfg.heartbeat_min);
    return web_send_json(req, r);
}

static esp_err_t integrations_post_handler(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *body = web_read_json(req, 2048);
    if (!body) return web_send_err(req, "invalid JSON body", 400);

    // Overlay present fields onto the current config. An explicitly empty
    // api_key/webhook_url clears (= disables) that feature.
    integrations_cfg_t cfg;
    integrations_get(&cfg);

    const cJSON *it;
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "api_key"))) {
        if (strlen(it->valuestring) > INTEGR_API_KEY_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "api_key too long (max 64)", 400);
        }
        snprintf(cfg.api_key, sizeof(cfg.api_key), "%s", it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "webhook_url"))) {
        if (strlen(it->valuestring) > INTEGR_URL_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "webhook_url too long (max 160)", 400);
        }
        snprintf(cfg.webhook_url, sizeof(cfg.webhook_url), "%s", it->valuestring);
    }

    // Browser login. An absent ui_pass keeps the stored one, so the UI can
    // rename the user or toggle the login off and on without the password
    // making a round trip through the browser each time.
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "ui_user"))) {
        if (strlen(it->valuestring) > INTEGR_UI_USER_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "ui_user too long (max 32)", 400);
        }
        snprintf(cfg.ui_user, sizeof(cfg.ui_user), "%s", it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "ui_pass"))) {
        if (strlen(it->valuestring) > INTEGR_UI_PASS_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "ui_pass too long (max 64)", 400);
        }
        snprintf(cfg.ui_pass, sizeof(cfg.ui_pass), "%s", it->valuestring);
    }
    if (cJSON_IsBool(it = cJSON_GetObjectItem(body, "ui_auth"))) {
        cfg.ui_auth = cJSON_IsTrue(it);
    }
    // Refused rather than silently downgraded: a client that asked for a
    // login and got {"ok":true} must not end up with an open device. The
    // username is allowed to be empty (browsers accept an empty user), the
    // password is not, or there would be nothing to check.
    if (cfg.ui_auth && cfg.ui_pass[0] == '\0') {
        cJSON_Delete(body);
        return web_send_err(req, "ui_auth needs a ui_pass", 400);
    }
    // The device name moved to the System tab (POST /api/system/device_name),
    // where it is validated strictly - it is a hostname and an SSID now. This
    // field stays for existing clients and writes the same single value, but
    // leniently: whatever it is given is cleaned to the allowed charset rather
    // than rejected, so a script written against 2.1 keeps working. The GET
    // above reports the cleaned result.
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "device_name"))) {
        if (strlen(it->valuestring) > INTEGR_NAME_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "device_name too long (max 32)", 400);
        }
        snprintf(cfg.device_name, sizeof(cfg.device_name), "%s", it->valuestring);
    }
    if ((it = cJSON_GetObjectItem(body, "webhook_events")) != NULL) {
        if (!events_from_json(it, &cfg.webhook_events)) {
            cJSON_Delete(body);
            return web_send_err(req,
                                "webhook_events must be an array of: boot, "
                                "ip_change, ota_result, schedule_on, "
                                "schedule_off, value_stale, value_recovered",
                                400);
        }
    }

    // Outbound webhook auth. Same contract as the other secrets: an absent or
    // empty secret / header value keeps the stored one; only an explicit
    // clear_* flag removes it. The header name is plain config and may be
    // set to "" directly, which also stops the header being sent. Clears run
    // first, so one request can clear and set a new value.
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "clear_webhook_secret"))) {
        cfg.webhook_secret[0] = '\0';
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "clear_webhook_hdr"))) {
        cfg.webhook_hdr_name[0]  = '\0';
        cfg.webhook_hdr_value[0] = '\0';
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "webhook_secret")) &&
        it->valuestring[0] != '\0') {
        if (strlen(it->valuestring) > INTEGR_SECRET_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "webhook_secret too long (max 128)", 400);
        }
        snprintf(cfg.webhook_secret, sizeof(cfg.webhook_secret), "%s",
                 it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "webhook_hdr_name"))) {
        if (strlen(it->valuestring) > INTEGR_HDR_NAME_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "webhook_hdr_name too long (max 48)", 400);
        }
        snprintf(cfg.webhook_hdr_name, sizeof(cfg.webhook_hdr_name), "%s",
                 it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "webhook_hdr_value")) &&
        it->valuestring[0] != '\0') {
        if (strlen(it->valuestring) > INTEGR_SECRET_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "webhook_hdr_value too long (max 128)", 400);
        }
        snprintf(cfg.webhook_hdr_value, sizeof(cfg.webhook_hdr_value), "%s",
                 it->valuestring);
    }
    if ((it = cJSON_GetObjectItem(body, "heartbeat_min")) != NULL) {
        if (!cJSON_IsNumber(it) || it->valuedouble < 0 ||
            it->valuedouble > INTEGR_HEARTBEAT_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "heartbeat_min must be 0..1440", 400);
        }
        cfg.heartbeat_min = (uint16_t)it->valuedouble;
    }
    cJSON_Delete(body);

    esp_err_t err = integrations_set(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving integrations failed: %s", esp_err_to_name(err));
        return web_send_err(req, "saving integrations failed", 500);
    }
    return web_send_ok(req);
}

// ---- /api/wireguard --------------------------------------------------------

static esp_err_t wireguard_get_handler(httpd_req_t *req)
{
    wg_config_t cfg;
    wg_status_t st;
    wg_client_get_config(&cfg);
    wg_client_get_status(&st);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", cfg.enabled);
    cJSON_AddBoolToObject(r, "private_key_set", cfg.private_key[0] != '\0');
    cJSON_AddStringToObject(r, "peer_public_key", cfg.peer_public_key);
    cJSON_AddStringToObject(r, "endpoint_host", cfg.endpoint_host);
    cJSON_AddNumberToObject(r, "endpoint_port", cfg.endpoint_port);
    cJSON_AddStringToObject(r, "address", cfg.address);
    cJSON_AddStringToObject(r, "allowed_ips", cfg.allowed_ips);
    cJSON_AddNumberToObject(r, "keepalive_s", cfg.keepalive_s);

    cJSON *status = cJSON_AddObjectToObject(r, "status");
    cJSON_AddBoolToObject(status, "compiled_in", st.compiled_in);
    cJSON_AddBoolToObject(status, "up", st.up);
    cJSON_AddBoolToObject(status, "held", st.held);
    cJSON_AddNumberToObject(status, "last_handshake", (double)st.last_handshake);
    // The public half is not a secret - the peer at the other end needs it -
    // and there is no other way to read it off the device.
    cJSON_AddStringToObject(status, "public_key", st.public_key);
    cJSON_AddStringToObject(status, "endpoint_ip", st.endpoint_ip);
    cJSON_AddNumberToObject(status, "attempts", st.attempts);
    cJSON_AddNumberToObject(status, "stage", st.stage);
    cJSON_AddStringToObject(status, "netif_ip", st.netif_ip);
    cJSON_AddStringToObject(status, "netif_netmask", st.netif_netmask);
    cJSON_AddBoolToObject(status, "netif_up", st.netif_up);
    cJSON_AddBoolToObject(status, "netif_link_up", st.netif_link_up);
    cJSON_AddNumberToObject(status, "rx_accepted", st.rx_accepted);
    cJSON_AddNumberToObject(status, "rx_dropped", st.rx_dropped);
    return web_send_json(req, r);
}

static esp_err_t wireguard_post_handler(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *body = web_read_json(req, 1024);
    if (!body) return web_send_err(req, "invalid JSON body", 400);

    wg_config_t cfg;
    wg_client_get_config(&cfg);

    const cJSON *it;
    // A full reset needs its own flag: every field here is "absent keeps the
    // stored value", and the private key deliberately cannot be cleared by
    // sending an empty string, so there is otherwise no way back to blank.
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "reset"))) {
        cJSON_Delete(body);
        memset(&cfg, 0, sizeof(cfg));
        cfg.endpoint_port = 51820;
        cfg.keepalive_s   = 25;
        esp_err_t rerr = wg_client_apply(&cfg);
        if (rerr == ESP_ERR_NOT_SUPPORTED) {
            return web_send_err(req, "WireGuard support not compiled in", 501);
        }
        if (rerr != ESP_OK) {
            ESP_LOGE(TAG, "wg reset failed: %s", esp_err_to_name(rerr));
            return web_send_err(req, "clearing WireGuard config failed", 500);
        }
        return web_send_ok(req);
    }
    if (cJSON_IsBool(it = cJSON_GetObjectItem(body, "enabled"))) {
        cfg.enabled = cJSON_IsTrue(it);
    }
    // An absent or empty private_key keeps the stored one, so the UI can
    // re-save the form without forcing the user to paste the key again.
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "private_key")) &&
        it->valuestring[0] != '\0') {
        if (strlen(it->valuestring) > WG_KEY_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "private_key too long", 400);
        }
        snprintf(cfg.private_key, sizeof(cfg.private_key), "%s", it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "peer_public_key"))) {
        if (strlen(it->valuestring) > WG_KEY_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "peer_public_key too long", 400);
        }
        snprintf(cfg.peer_public_key, sizeof(cfg.peer_public_key), "%s",
                 it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "endpoint_host"))) {
        if (strlen(it->valuestring) > WG_HOST_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "endpoint_host too long", 400);
        }
        snprintf(cfg.endpoint_host, sizeof(cfg.endpoint_host), "%s",
                 it->valuestring);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(body, "endpoint_port"))) {
        if (it->valueint < 0 || it->valueint > 0xFFFF) {
            cJSON_Delete(body);
            return web_send_err(req, "endpoint_port must be 0..65535", 400);
        }
        cfg.endpoint_port = (uint16_t)it->valueint;
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "address"))) {
        if (strlen(it->valuestring) > WG_ADDR_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "address too long", 400);
        }
        snprintf(cfg.address, sizeof(cfg.address), "%s", it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "allowed_ips"))) {
        if (strlen(it->valuestring) > WG_ALLOWED_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "allowed_ips too long", 400);
        }
        snprintf(cfg.allowed_ips, sizeof(cfg.allowed_ips), "%s", it->valuestring);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(body, "keepalive_s"))) {
        if (it->valueint < 0 || it->valueint > 0xFFFF) {
            cJSON_Delete(body);
            return web_send_err(req, "keepalive_s must be 0..65535", 400);
        }
        cfg.keepalive_s = (uint16_t)it->valueint;
    }
    cJSON_Delete(body);

    esp_err_t err = wg_client_apply(&cfg);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return web_send_err(req, "WireGuard support not compiled in", 501);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wg_client_apply failed: %s", esp_err_to_name(err));
        return web_send_err(req, "applying WireGuard config failed", 500);
    }
    return web_send_ok(req);
}

// ---- /api/mqtt ---------------------------------------------------------------

static esp_err_t mqtt_get_handler(httpd_req_t *req)
{
    mqtt_cfg_t cfg;
    mqtt_link_status_t st;
    mqtt_link_get_config(&cfg);
    mqtt_link_get_status(&st);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", cfg.enabled);
    cJSON_AddStringToObject(r, "uri", cfg.uri);
    cJSON_AddStringToObject(r, "username", cfg.username);
    // Secrets policy (DESIGN.md section 10): report only whether one is set.
    cJSON_AddBoolToObject(r, "password_set", cfg.password[0] != '\0');
    cJSON_AddStringToObject(r, "base_topic", cfg.base_topic);
    cJSON_AddBoolToObject(r, "ha_discovery", cfg.ha_discovery);
    cJSON_AddStringToObject(r, "ha_prefix", cfg.ha_prefix);

    cJSON *status = cJSON_AddObjectToObject(r, "status");
    cJSON_AddBoolToObject(status, "compiled_in", st.compiled_in);
    cJSON_AddBoolToObject(status, "connected", st.connected);
    return web_send_json(req, r);
}

static esp_err_t mqtt_post_handler(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *body = web_read_json(req, 1024);
    if (!body) return web_send_err(req, "invalid JSON body", 400);

    mqtt_cfg_t cfg;
    mqtt_link_get_config(&cfg);

    const cJSON *it;
    if (cJSON_IsBool(it = cJSON_GetObjectItem(body, "enabled"))) {
        cfg.enabled = cJSON_IsTrue(it);
    }
    if (cJSON_IsBool(it = cJSON_GetObjectItem(body, "ha_discovery"))) {
        cfg.ha_discovery = cJSON_IsTrue(it);
    }
    struct { const char *key; char *dst; size_t sz; } strs[] = {
        {"uri",        cfg.uri,        sizeof(cfg.uri)},
        {"username",   cfg.username,   sizeof(cfg.username)},
        {"base_topic", cfg.base_topic, sizeof(cfg.base_topic)},
        {"ha_prefix",  cfg.ha_prefix,  sizeof(cfg.ha_prefix)},
    };
    for (unsigned i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
        if (!cJSON_IsString(it = cJSON_GetObjectItem(body, strs[i].key))) continue;
        if (strlen(it->valuestring) >= strs[i].sz) {
            cJSON_Delete(body);
            return web_send_err(req, "field too long", 400);
        }
        snprintf(strs[i].dst, strs[i].sz, "%s", it->valuestring);
    }
    // An absent or empty password keeps the stored one, so the UI can
    // re-save the form without forcing the user to paste it again (same
    // contract as the WireGuard private key). A stored password is harmless
    // once the username is cleared: anonymous connects ignore it.
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "password")) &&
        it->valuestring[0] != '\0') {
        if (strlen(it->valuestring) > MQTT_PASS_MAX) {
            cJSON_Delete(body);
            return web_send_err(req, "password too long", 400);
        }
        snprintf(cfg.password, sizeof(cfg.password), "%s", it->valuestring);
    }
    cJSON_Delete(body);

    if (cfg.enabled && cfg.uri[0] == '\0') {
        return web_send_err(req, "uri required to enable MQTT", 400);
    }

    esp_err_t err = mqtt_link_apply(&cfg);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return web_send_err(req, "MQTT support not compiled in", 501);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt_link_apply failed: %s", esp_err_to_name(err));
        return web_send_err(req, "applying MQTT config failed", 500);
    }
    return web_send_ok(req);
}

// ---- registration ----------------------------------------------------------

void api_integrations_register(httpd_handle_t srv)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/integrations", .method = HTTP_GET,  .handler = integrations_get_handler},
        {.uri = "/api/integrations", .method = HTTP_POST, .handler = integrations_post_handler},
        {.uri = "/api/wireguard",    .method = HTTP_GET,  .handler = wireguard_get_handler},
        {.uri = "/api/wireguard",    .method = HTTP_POST, .handler = wireguard_post_handler},
        {.uri = "/api/mqtt",         .method = HTTP_GET,  .handler = mqtt_get_handler},
        {.uri = "/api/mqtt",         .method = HTTP_POST, .handler = mqtt_post_handler},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        web_register_uri(srv, &uris[i]);
    }
}
