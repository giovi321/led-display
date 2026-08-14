// /api/wifi/* handlers: scan, connect, reset. Endpoint ownership:
// api_handlers.h; response shapes: DESIGN.md section 5.

#include "esp_log.h"

#include "api_handlers.h"
#include "web_common.h"
#include "wifi_manager.h"

static const char *TAG = "api_wifi";

#define BODY_MAX 2048

// In AP (captive-portal setup) mode the WiFi endpoints stay keyless: the
// user is on the setup sheet and could not present an API key yet
// (DESIGN.md section 5). Everywhere else the normal POST auth applies.
static esp_err_t wifi_require_auth(httpd_req_t *req)
{
    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);
    if (w.mode == WIFI_MGR_MODE_AP) {
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

    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(r, "networks");
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < n; i++) {
            cJSON *item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "ssid", (const char *)records[i].ssid);
            cJSON_AddNumberToObject(item, "rssi", records[i].rssi);
            cJSON_AddNumberToObject(item, "auth", records[i].authmode);
            cJSON_AddItemToArray(arr, item);
        }
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        cJSON_AddStringToObject(r, "error", esp_err_to_name(err));
    }
    return web_send_json(req, r);
}

static esp_err_t wifi_connect_post(httpd_req_t *req)
{
    if (wifi_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const cJSON *ssid = cJSON_GetObjectItem(json, "ssid");
    const cJSON *pass = cJSON_GetObjectItem(json, "password");
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(json);
        return web_send_err(req, "ssid required", 400);
    }
    const char *p = cJSON_IsString(pass) ? pass->valuestring : "";

    // 802.11 limits: SSID 32 bytes, WPA passphrase 64. NVS would store
    // longer strings, but the boot loader reads them into fixed buffers and
    // fails, leaving the device stuck in setup mode after the reboot.
    if (strlen(ssid->valuestring) > 32 || strlen(p) > 64) {
        cJSON_Delete(json);
        return web_send_err(req, "ssid max 32 chars, password max 64", 400);
    }

    web_send_ok(req);  // reply before we reboot
    wifi_manager_save_and_reboot(ssid->valuestring, p);
    cJSON_Delete(json);
    return ESP_OK;
}

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
        {.uri = "/api/wifi/scan",    .method = HTTP_GET,  .handler = wifi_scan_get},
        {.uri = "/api/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_post},
        {.uri = "/api/wifi/reset",   .method = HTTP_POST, .handler = wifi_reset_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
}
