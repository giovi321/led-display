// /api/status, /api/system/{reboot,boot_screen,factory_reset} and
// /api/schedule handlers. Endpoint ownership: api_handlers.h; response
// shapes: DESIGN.md section 5.

#include <string.h>

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "api_handlers.h"
#include "display.h"
#include "fw_version.h"
#include "integrations.h"
#include "led_driver.h"
#include "mapping.h"
#include "ota_update.h"
#include "scheduler.h"
#include "settings.h"
#include "web_common.h"
#include "wg_client.h"
#include "wifi_manager.h"

static const char *TAG = "api_system";

#define BODY_MAX 2048

static const char *mode_str(wifi_mgr_mode_t m)
{
    switch (m) {
        case WIFI_MGR_MODE_STA: return "sta";
        case WIFI_MGR_MODE_AP:  return "ap";
        default:                return "boot";
    }
}

static esp_err_t status_get(httpd_req_t *req)
{
    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);

    char text[DISPLAY_TEXT_MAX];
    display_color_t col;
    bool scroll;
    uint8_t speed;
    uint8_t scale;
    display_get_state(text, sizeof(text), &col, &scroll, &speed, &scale);

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const char *chip_name =
        chip.model == CHIP_ESP32S3 ? "ESP32-S3" :
        chip.model == CHIP_ESP32   ? "ESP32"   : "esp";

    cJSON *r    = cJSON_CreateObject();
    cJSON *wifi = cJSON_AddObjectToObject(r, "wifi");
    cJSON_AddStringToObject(wifi, "mode", mode_str(w.mode));
    cJSON_AddStringToObject(wifi, "ssid", w.ssid);
    cJSON_AddStringToObject(wifi, "ip",   w.ip);
    cJSON_AddNumberToObject(wifi, "rssi", w.rssi);
    cJSON_AddBoolToObject(wifi,   "connected", w.connected);

    cJSON *d = cJSON_AddObjectToObject(r, "display");
    cJSON_AddStringToObject(d, "text", text);
    cJSON_AddBoolToObject(d,   "scroll", scroll);
    cJSON_AddNumberToObject(d, "speed", speed);
    cJSON_AddNumberToObject(d, "scale", scale);
    cJSON_AddNumberToObject(d, "brightness", led_driver_get_brightness());
    cJSON *c = cJSON_AddObjectToObject(d, "color");
    cJSON_AddNumberToObject(c, "r", col.r);
    cJSON_AddNumberToObject(c, "g", col.g);
    cJSON_AddNumberToObject(c, "b", col.b);

    cJSON *s = cJSON_AddObjectToObject(r, "system");
    cJSON_AddStringToObject(s, "fw", FW_VERSION);
    cJSON_AddStringToObject(s, "chip", chip_name);
    cJSON_AddNumberToObject(s, "cores", chip.cores);
    cJSON_AddNumberToObject(s, "rev",  chip.revision);
    cJSON_AddNumberToObject(s, "heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(s, "uptime", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(s, "brightness_cap", led_driver_get_brightness_cap());

    // Surface the saved boot-screen choice so the System tab can reflect it.
    settings_t cfg;
    settings_load(&cfg);
    const char *bs = "text";
    if      (cfg.boot_screen == BOOT_SHOW_IP)   bs = "ip";
    else if (cfg.boot_screen == BOOT_SHOW_NONE) bs = "none";
    cJSON_AddStringToObject(s, "boot_screen", bs);

    // Active layout summary; the full config comes from GET /api/layout.
    mapping_config_t map;
    led_driver_get_config(&map);
    cJSON *lay = cJSON_AddObjectToObject(r, "layout");
    cJSON_AddNumberToObject(lay, "rows", map.rows);
    cJSON_AddNumberToObject(lay, "cols", map.cols);
    cJSON_AddNumberToObject(lay, "panel_w", map.panel.w);
    cJSON_AddNumberToObject(lay, "panel_h", map.panel.h);
    cJSON_AddNumberToObject(lay, "width",  mapping_width(&map));
    cJSON_AddNumberToObject(lay, "height", mapping_height(&map));
    cJSON_AddNumberToObject(lay, "panels", mapping_panels(&map));

    // Seconds since the last POST /api/value; -1 = never received one.
    cJSON_AddNumberToObject(r, "value_age_s", (double)integrations_value_age_s());

    wg_status_t wg;
    wg_client_get_status(&wg);
    cJSON *wgj = cJSON_AddObjectToObject(r, "wireguard");
    cJSON_AddBoolToObject(wgj, "compiled_in", wg.compiled_in);
    cJSON_AddBoolToObject(wgj, "enabled", wg.enabled);
    cJSON_AddBoolToObject(wgj, "up", wg.up);
    cJSON_AddNumberToObject(wgj, "last_handshake", (double)wg.last_handshake);

    ota_status_t ota;
    ota_get_status(&ota);
    cJSON *oj = cJSON_AddObjectToObject(r, "ota");
    cJSON_AddStringToObject(oj, "slot", ota.running_slot);
    cJSON_AddStringToObject(oj, "version", ota.running_version);
    cJSON_AddBoolToObject(oj, "in_progress", ota.in_progress);

    // true = the schedule currently allows the display to be on.
    cJSON_AddBoolToObject(r, "schedule_active", !scheduler_display_is_off());

    return web_send_json(req, r);
}

static esp_err_t system_reboot_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    web_send_ok(req);  // reply before we reboot
    ESP_LOGI(TAG, "reboot requested");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t system_boot_screen_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const cJSON *m = cJSON_GetObjectItem(json, "mode");
    const char *name = cJSON_IsString(m) ? m->valuestring : "";

    boot_screen_t which = BOOT_SHOW_TEXT;
    if      (!strcmp(name, "text")) which = BOOT_SHOW_TEXT;
    else if (!strcmp(name, "ip"))   which = BOOT_SHOW_IP;
    else if (!strcmp(name, "none")) which = BOOT_SHOW_NONE;
    else {
        cJSON_Delete(json);
        return web_send_err(req, "unknown mode", 400);
    }
    cJSON_Delete(json);

    settings_save_boot_screen(which);
    return web_send_ok(req);
}

// Wipes the "cfg" NVS namespace only: WiFi credentials live in their own
// namespace and survive on purpose, so a factory reset can't strand a
// wall-mounted device off the network (use /api/wifi/reset for that).
static esp_err_t system_factory_reset_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    esp_err_t err = settings_erase();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset: settings_erase failed: %s", esp_err_to_name(err));
        return web_send_err(req, "erase failed", 500);
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "note", "display settings wiped; wifi kept");
    web_send_json(req, r);  // reply before we reboot

    ESP_LOGI(TAG, "factory reset: cfg namespace erased, rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

static esp_err_t schedule_get(httpd_req_t *req)
{
    schedule_t sc;
    scheduler_get(&sc);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", sc.enabled);
    cJSON_AddNumberToObject(r, "on_hour", sc.on_hour);
    cJSON_AddNumberToObject(r, "on_min", sc.on_min);
    cJSON_AddNumberToObject(r, "off_hour", sc.off_hour);
    cJSON_AddNumberToObject(r, "off_min", sc.off_min);
    cJSON_AddNumberToObject(r, "days", sc.days);
    cJSON_AddNumberToObject(r, "utc_offset", sc.utc_offset);
    return web_send_json(req, r);
}

static esp_err_t schedule_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    schedule_t sc;
    scheduler_get(&sc);

    const cJSON *en = cJSON_GetObjectItem(json, "enabled");
    const cJSON *oh = cJSON_GetObjectItem(json, "on_hour");
    const cJSON *om = cJSON_GetObjectItem(json, "on_min");
    const cJSON *fh = cJSON_GetObjectItem(json, "off_hour");
    const cJSON *fm = cJSON_GetObjectItem(json, "off_min");
    const cJSON *d  = cJSON_GetObjectItem(json, "days");
    const cJSON *u  = cJSON_GetObjectItem(json, "utc_offset");

    // Reject out-of-range values instead of letting the uint8/int16 casts
    // wrap: a stored on_hour of 300 -> 44 would persistently corrupt the
    // schedule while still replying ok. Same limits as the import path.
    if ((cJSON_IsNumber(oh) && (oh->valueint < 0 || oh->valueint > 23)) ||
        (cJSON_IsNumber(fh) && (fh->valueint < 0 || fh->valueint > 23)) ||
        (cJSON_IsNumber(om) && (om->valueint < 0 || om->valueint > 59)) ||
        (cJSON_IsNumber(fm) && (fm->valueint < 0 || fm->valueint > 59)) ||
        (cJSON_IsNumber(u)  && (u->valueint < -14 * 60 || u->valueint > 14 * 60))) {
        cJSON_Delete(json);
        return web_send_err(req, "hours 0-23, minutes 0-59, utc_offset -840..840", 400);
    }

    if (cJSON_IsBool(en))   sc.enabled    = cJSON_IsTrue(en);
    if (cJSON_IsNumber(oh)) sc.on_hour    = (uint8_t)oh->valueint;
    if (cJSON_IsNumber(om)) sc.on_min     = (uint8_t)om->valueint;
    if (cJSON_IsNumber(fh)) sc.off_hour   = (uint8_t)fh->valueint;
    if (cJSON_IsNumber(fm)) sc.off_min    = (uint8_t)fm->valueint;
    if (cJSON_IsNumber(d))  sc.days       = (uint8_t)d->valueint & SCHED_DAYS_ALL;
    if (cJSON_IsNumber(u))  sc.utc_offset = (int16_t)u->valueint;

    cJSON_Delete(json);
    scheduler_set(&sc);
    return web_send_ok(req);
}

void api_system_register(httpd_handle_t srv)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/status",               .method = HTTP_GET,  .handler = status_get},
        {.uri = "/api/system/reboot",        .method = HTTP_POST, .handler = system_reboot_post},
        {.uri = "/api/system/boot_screen",   .method = HTTP_POST, .handler = system_boot_screen_post},
        {.uri = "/api/system/factory_reset", .method = HTTP_POST, .handler = system_factory_reset_post},
        {.uri = "/api/schedule",             .method = HTTP_GET,  .handler = schedule_get},
        {.uri = "/api/schedule",             .method = HTTP_POST, .handler = schedule_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
}
