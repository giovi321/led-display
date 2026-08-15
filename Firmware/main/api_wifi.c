// /api/wifi/* handlers: scan, saved-network list/add/delete, connect, reset.
// Endpoint ownership: api_handlers.h; response shapes: DESIGN.md section 5.
// Passwords are write-only here - no endpoint ever returns one.

#include <string.h>

#include "esp_log.h"

#include "api_handlers.h"
#include "web_common.h"
#include "wifi_manager.h"

static const char *TAG = "api_wifi";

#define BODY_MAX 2048

// The WiFi endpoints stay keyless only on the setup AP of a device that has no
// saved network yet: that is genuine first-time provisioning, where nobody could
// have configured a key. A provisioned device that raised the setup AP after a
// long outage still requires it - that AP is open, so the exception would
// otherwise hand any passer-by the ability to wipe its credentials or move it
// onto their own network (DESIGN.md section 5).
static esp_err_t wifi_require_auth(httpd_req_t *req)
{
    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);
    wifi_mgr_saved_t saved[WIFI_MGR_MAX_NETS];
    if (w.mode == WIFI_MGR_MODE_AP &&
        wifi_manager_saved_list(saved, WIFI_MGR_MAX_NETS) == 0) {
        return ESP_OK;
    }
    return web_require_auth(req);
}

static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    // Static so the ~1.6 KB record array doesn't live on the httpd stack;
    // the server handles requests one at a time, so no reentrancy issue.
    static wifi_ap_record_t records[20];
    uint16_t n = sizeof(records) / sizeof(records[0]);
    esp_err_t err = wifi_manager_scan(records, &n);

    // Mark the ones the device already knows so the UI can skip re-typing a
    // password for them.
    wifi_mgr_saved_t saved[WIFI_MGR_MAX_NETS];
    int n_saved = wifi_manager_saved_list(saved, WIFI_MGR_MAX_NETS);

    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "networks");
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < n; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "ssid", (const char *)records[i].ssid);
            cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
            cJSON_AddNumberToObject(item, "auth", records[i].authmode);
            bool known = false;
            for (int k = 0; k < n_saved; k++) {
                if (strcmp(saved[k].ssid, (const char *)records[i].ssid) == 0) {
                    known = true;
                    break;
                }
            }
            cJSON_AddBoolToObject(item, "saved", known);
            cJSON_AddItemToArray(arr, item);
        }
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        cJSON_AddStringToObject(r, "error", esp_err_to_name(err));
    }
    return web_send_json(req, r);
}

// GET /api/wifi/networks -> {"networks":[{"ssid","connected"}],"count","max"}
static esp_err_t wifi_networks_get(httpd_req_t *req)
{
    wifi_mgr_saved_t saved[WIFI_MGR_MAX_NETS];
    int n = wifi_manager_saved_list(saved, WIFI_MGR_MAX_NETS);

    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "networks");
    for (int i = 0; i < n; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", saved[i].ssid);
        cJSON_AddBoolToObject(item, "connected", saved[i].connected);
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddNumberToObject(r, "count", n);
    cJSON_AddNumberToObject(r, "max", WIFI_MGR_MAX_NETS);
    return web_send_json(req, r);
}

// Pull ssid/password out of a request body. Returns NULL when the SSID is
// missing or empty; the callers apply the length limits.
static const char *creds_from_body(const cJSON *json, const char **out_pass)
{
    const cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(json, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') return NULL;
    *out_pass = cJSON_IsString(pass) ? pass->valuestring : "";
    return ssid->valuestring;
}

static esp_err_t wifi_networks_post(httpd_req_t *req)
{
    if (wifi_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const char *pass = "";
    const char *ssid = creds_from_body(json, &pass);
    if (!ssid) {
        cJSON_Delete(json);
        return web_send_err(req, "ssid required", 400);
    }
    if (strlen(ssid) > 32 || strlen(pass) > 64) {
        cJSON_Delete(json);
        return web_send_err(req, "ssid max 32 chars, password max 64", 400);
    }

    esp_err_t err = wifi_manager_saved_add(ssid, pass);
    cJSON_Delete(json);
    if (err == ESP_ERR_NO_MEM) {
        return web_send_err(req, "all network slots are in use", 409);
    }
    if (err != ESP_OK) return web_send_err(req, "could not save", 500);

    wifi_mgr_saved_t saved[WIFI_MGR_MAX_NETS];
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "count",
                            wifi_manager_saved_list(saved, WIFI_MGR_MAX_NETS));
    return web_send_json(req, r);
}

static esp_err_t wifi_networks_delete_post(httpd_req_t *req)
{
    if (wifi_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(json);
        return web_send_err(req, "ssid required", 400);
    }
    esp_err_t err = wifi_manager_saved_delete(ssid->valuestring);
    cJSON_Delete(json);
    if (err == ESP_ERR_NOT_FOUND) return web_send_err(req, "not saved", 404);
    if (err != ESP_OK) return web_send_err(req, "could not save", 500);

    wifi_mgr_saved_t saved[WIFI_MGR_MAX_NETS];
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddNumberToObject(r, "count",
                            wifi_manager_saved_list(saved, WIFI_MGR_MAX_NETS));
    return web_send_json(req, r);
}

static esp_err_t wifi_connect_post(httpd_req_t *req)
{
    if (wifi_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const char *p = "";
    const char *ssid = creds_from_body(json, &p);
    if (!ssid) {
        cJSON_Delete(json);
        return web_send_err(req, "ssid required", 400);
    }

    // 802.11 limits: SSID 32 bytes, WPA passphrase 64. NVS would store
    // longer strings, but the boot loader reads them into fixed buffers and
    // fails, leaving the device stuck in setup mode after the reboot.
    if (strlen(ssid) > 32 || strlen(p) > 64) {
        cJSON_Delete(json);
        return web_send_err(req, "ssid max 32 chars, password max 64", 400);
    }

    // Adds or updates the network (the other saved ones stay) and reboots so
    // the new credentials take effect from a clean boot.
    web_send_ok(req);  // reply before we reboot
    wifi_manager_save_and_reboot(ssid, p);
    cJSON_Delete(json);
    return ESP_OK;
}

// Clears EVERY saved network, not just the active one.
static esp_err_t wifi_reset_post(httpd_req_t *req)
{
    if (wifi_require_auth(req) != ESP_OK) return ESP_OK;

    web_send_ok(req);  // reply before we reboot
    wifi_manager_reset_and_reboot();
    return ESP_OK;
}

void api_wifi_register(httpd_handle_t srv)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/wifi/scan",     .method = HTTP_GET,  .handler = wifi_scan_get},
        {.uri = "/api/wifi/networks", .method = HTTP_GET,  .handler = wifi_networks_get},
        {.uri = "/api/wifi/networks", .method = HTTP_POST, .handler = wifi_networks_post},
        {.uri = "/api/wifi/networks/delete", .method = HTTP_POST,
                                             .handler = wifi_networks_delete_post},
        {.uri = "/api/wifi/connect",  .method = HTTP_POST, .handler = wifi_connect_post},
        {.uri = "/api/wifi/reset",    .method = HTTP_POST, .handler = wifi_reset_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        web_register_uri(srv, &uris[i]);
    }
}
