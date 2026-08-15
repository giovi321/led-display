// /api/status, /api/system/{reboot,boot_screen,factory_reset} and
// /api/schedule handlers. Endpoint ownership: api_handlers.h; response
// shapes: DESIGN.md section 5.

#include <stdio.h>
#include <string.h>

#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "api_handlers.h"
#include "devname.h"
#include "display.h"
#include "fw_version.h"
#include "integrations.h"
#include "led_driver.h"
#include "light_sensor.h"
#include "mapping.h"
#include "mqtt_link.h"
#include "ota_update.h"
#include "scheduler.h"
#include "settings.h"
#include "tz.h"
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
    uint8_t scroll_mode;
    uint8_t speed;
    uint8_t scale;
    display_get_state(text, sizeof(text), &col, &scroll_mode, &speed, &scale);
    uint8_t font_id;
    bool prop;
    display_get_font(&font_id, &prop);

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
    // Reported in every mode: the System tab shows which SSID the setup
    // network would appear under, without knowing how the name is resolved.
    cJSON_AddStringToObject(wifi, "ap_ssid", w.ap_ssid);

    cJSON *d = cJSON_AddObjectToObject(r, "display");
    cJSON_AddStringToObject(d, "text", text);
    // Both scroll spellings: the bool for pre-2.2 clients, the mode string
    // for everyone else.
    cJSON_AddBoolToObject(d,   "scroll", scroll_mode != 0);
    static const char *SCROLL_NAMES[] = {"off", "restart", "continuous"};
    cJSON_AddStringToObject(d, "scroll_mode",
                            SCROLL_NAMES[scroll_mode <= 2 ? scroll_mode : 1]);
    cJSON_AddNumberToObject(d, "speed", speed);
    cJSON_AddNumberToObject(d, "scale", scale);
    cJSON_AddStringToObject(d, "font", font_name(font_id));
    cJSON_AddStringToObject(d, "spacing", prop ? "proportional" : "monospace");
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

    // The single device name: hostname, setup-AP SSID and webhook "device".
    // Always the resolved value, so a client never has to know the default.
    char dname[INTEGR_NAME_MAX + 1];
    integrations_get_device_name(dname, sizeof(dname));
    cJSON_AddStringToObject(s, "device_name", dname);
    // Empty when no name is stored: the UI shows its placeholder instead of
    // pre-filling the field with a default the user never chose.
    char dstored[DEVNAME_MAX + 1];
    settings_load_device_name(dstored, sizeof(dstored));
    cJSON_AddStringToObject(s, "device_name_stored", dstored);

    // Surface the saved boot-screen choice so the System tab can reflect it.
    settings_t cfg;
    settings_load(&cfg);
    // Content source: what the panel is wired to show. display.text above is
    // whatever is on screen right now, whichever source produced it.
    cJSON_AddStringToObject(d, "source",
                            cfg.source == DISPLAY_SRC_INTEGRATION
                                ? "integration" : "static");
    // display.brightness is what the arbiter last applied (0 in a blanked
    // off-window, halved by the staleness guard); the stored user level is a
    // separate field, so the UI slider can be an input without writing the
    // applied value back.
    cJSON_AddNumberToObject(d, "brightness_stored", cfg.brightness);
    // The persisted static text, distinct from display.text (which is
    // whatever is on screen - the pushed value in integration mode).
    cJSON_AddStringToObject(d, "text_stored", cfg.text);
    const char *bs = "text";
    if      (cfg.boot_screen == BOOT_SHOW_IP)      bs = "ip";
    else if (cfg.boot_screen == BOOT_SHOW_NONE)    bs = "none";
    else if (cfg.boot_screen == BOOT_SHOW_IP_THEN) bs = "ip_then_content";
    cJSON_AddStringToObject(s, "boot_screen", bs);
    static const char *STARTUP_NAMES[] = {"border", "blank", "content"};
    static const char *OFFLINE_NAMES[] = {"keep", "text", "blank", "dot"};
    cJSON_AddStringToObject(s, "startup_screen",
                            STARTUP_NAMES[cfg.startup_screen <= STARTUP_TEXT
                                          ? cfg.startup_screen : 0]);
    cJSON_AddStringToObject(s, "offline_screen",
                            OFFLINE_NAMES[cfg.offline_screen <= OFFLINE_DOT
                                          ? cfg.offline_screen : 0]);
    cJSON_AddStringToObject(s, "offline_text", cfg.offline_text);
    cJSON *oc = cJSON_AddObjectToObject(s, "offline_color");
    cJSON_AddNumberToObject(oc, "r", cfg.offline_color.r);
    cJSON_AddNumberToObject(oc, "g", cfg.offline_color.g);
    cJSON_AddNumberToObject(oc, "b", cfg.offline_color.b);
    cJSON_AddNumberToObject(s, "offline_x", cfg.offline_x);
    cJSON_AddNumberToObject(s, "offline_y", cfg.offline_y);
    cJSON_AddNumberToObject(s, "offline_brightness", cfg.offline_bright);

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
    cJSON_AddBoolToObject(wgj, "held", wg.held);
    cJSON_AddNumberToObject(wgj, "last_handshake", (double)wg.last_handshake);
    // Bring-up diagnostics: which step the last crash reboot died in, and how
    // close the lwIP thread (where the tunnel's crypto runs) came to
    // overflowing its stack.
    cJSON_AddStringToObject(wgj, "public_key", wg.public_key);
    cJSON_AddStringToObject(wgj, "endpoint_ip", wg.endpoint_ip);
    cJSON_AddStringToObject(wgj, "netif_ip", wg.netif_ip);
    cJSON_AddStringToObject(wgj, "netif_netmask", wg.netif_netmask);
    cJSON_AddNumberToObject(wgj, "attempts", wg.attempts);
    cJSON_AddNumberToObject(wgj, "stage", wg.stage);
    cJSON_AddNumberToObject(wgj, "last_crash_stage", wg.last_crash_stage);
    cJSON_AddNumberToObject(wgj, "last_crash_tcpip_free",
                            (double)wg.last_crash_tcpip_free);
    cJSON_AddNumberToObject(wgj, "tcpip_stack_free_min",
                            (double)wg.tcpip_stack_free_min);

    mqtt_link_status_t mq;
    mqtt_link_get_status(&mq);
    cJSON *mqj = cJSON_AddObjectToObject(r, "mqtt");
    cJSON_AddBoolToObject(mqj, "compiled_in", mq.compiled_in);
    cJSON_AddBoolToObject(mqj, "enabled", mq.enabled);
    cJSON_AddBoolToObject(mqj, "connected", mq.connected);

    ota_status_t ota;
    ota_get_status(&ota);
    cJSON *oj = cJSON_AddObjectToObject(r, "ota");
    cJSON_AddStringToObject(oj, "slot", ota.running_slot);
    cJSON_AddStringToObject(oj, "version", ota.running_version);
    cJSON_AddBoolToObject(oj, "in_progress", ota.in_progress);

    // Ambient sensor: `enabled` tells the Display tab that the brightness
    // slider is not in charge; `raw`/`level` are -1 until the first sample.
    light_sensor_status_t ls;
    light_sensor_get_status(&ls);
    cJSON *lsj = cJSON_AddObjectToObject(r, "light_sensor");
    cJSON_AddBoolToObject(lsj, "enabled", ls.enabled);
    cJSON_AddBoolToObject(lsj, "sampling", ls.sampling);
    cJSON_AddNumberToObject(lsj, "raw", ls.raw);
    cJSON_AddNumberToObject(lsj, "level", ls.level);

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
    else if (!strcmp(name, "ip_then_content")) which = BOOT_SHOW_IP_THEN;
    else {
        cJSON_Delete(json);
        return web_send_err(req, "unknown mode", 400);
    }
    cJSON_Delete(json);

    esp_err_t err = settings_save_boot_screen(which);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving boot screen failed: %s", esp_err_to_name(err));
        return web_send_err(req, "saving boot screen failed", 500);
    }
    return web_send_ok(req);
}

static uint8_t clamp_u8(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

// The two screens either side of the after-boot one: the startup splash, and
// what a lost WiFi link puts on the panel.
static esp_err_t system_screens_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    settings_t cfg;
    settings_load(&cfg);

    const cJSON *it;
    if (cJSON_IsString(it = cJSON_GetObjectItem(json, "startup_screen"))) {
        if      (!strcmp(it->valuestring, "border"))  cfg.startup_screen = STARTUP_BORDER;
        else if (!strcmp(it->valuestring, "blank"))   cfg.startup_screen = STARTUP_BLANK;
        else if (!strcmp(it->valuestring, "content")) cfg.startup_screen = STARTUP_TEXT;
        else {
            cJSON_Delete(json);
            return web_send_err(req, "startup_screen must be border|blank|content", 400);
        }
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(json, "offline_screen"))) {
        if      (!strcmp(it->valuestring, "keep"))  cfg.offline_screen = OFFLINE_KEEP;
        else if (!strcmp(it->valuestring, "text"))  cfg.offline_screen = OFFLINE_TEXT;
        else if (!strcmp(it->valuestring, "blank")) cfg.offline_screen = OFFLINE_BLANK;
        else if (!strcmp(it->valuestring, "dot"))   cfg.offline_screen = OFFLINE_DOT;
        else {
            cJSON_Delete(json);
            return web_send_err(req, "offline_screen must be keep|text|blank|dot", 400);
        }
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(json, "offline_text"))) {
        if (strlen(it->valuestring) > OFFLINE_TEXT_MAX) {
            cJSON_Delete(json);
            return web_send_err(req, "offline_text too long (max 63)", 400);
        }
        snprintf(cfg.offline_text, sizeof(cfg.offline_text), "%s", it->valuestring);
    }
    const cJSON *col = cJSON_GetObjectItem(json, "offline_color");
    if (cJSON_IsObject(col)) {
        const cJSON *cr = cJSON_GetObjectItem(col, "r");
        const cJSON *cg = cJSON_GetObjectItem(col, "g");
        const cJSON *cb = cJSON_GetObjectItem(col, "b");
        if (cJSON_IsNumber(cr)) cfg.offline_color.r = clamp_u8(cr->valueint);
        if (cJSON_IsNumber(cg)) cfg.offline_color.g = clamp_u8(cg->valueint);
        if (cJSON_IsNumber(cb)) cfg.offline_color.b = clamp_u8(cb->valueint);
    }
    // Position is validated against the panel that is actually configured, so
    // a marker can never be saved somewhere it would silently never appear.
    const int mw = led_driver_width(), mh = led_driver_height();
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(json, "offline_x"))) {
        if (it->valueint < 0 || it->valueint >= mw) {
            cJSON_Delete(json);
            char msg[64];
            snprintf(msg, sizeof(msg), "offline_x must be 0..%d", mw - 1);
            return web_send_err(req, msg, 400);
        }
        cfg.offline_x = (uint16_t)it->valueint;
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(json, "offline_y"))) {
        if (it->valueint < 0 || it->valueint >= mh) {
            cJSON_Delete(json);
            char msg[64];
            snprintf(msg, sizeof(msg), "offline_y must be 0..%d", mh - 1);
            return web_send_err(req, msg, 400);
        }
        cfg.offline_y = (uint16_t)it->valueint;
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(json, "offline_brightness"))) {
        cfg.offline_bright = clamp_u8(it->valueint);
    }
    cJSON_Delete(json);

    esp_err_t err = settings_save_screens(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving screens failed: %s", esp_err_to_name(err));
        return web_send_err(req, "saving screens failed", 500);
    }
    return web_send_ok(req);
}

// One name, three uses (hostname, setup-AP SSID, webhook "device"). An empty
// name clears it and every consumer goes back to its historical default.
static esp_err_t system_device_name_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const cJSON *n = cJSON_GetObjectItem(json, "name");
    if (!cJSON_IsString(n)) {
        cJSON_Delete(json);
        return web_send_err(req, "name required", 400);
    }
    char name[DEVNAME_MAX + 2];   // one over, so an over-long name still fails
    snprintf(name, sizeof(name), "%s", n->valuestring);
    cJSON_Delete(json);

    if (name[0] != '\0' && !devname_valid(name)) {
        return web_send_err(req,
                            "name must be 1-24 letters, digits or hyphens, "
                            "starting and ending with a letter or digit", 400);
    }

    esp_err_t err = integrations_set_device_name(name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving device name failed: %s", esp_err_to_name(err));
        return web_send_err(req, "saving device name failed", 500);
    }

    // Report what the name now resolves to, so the UI does not have to
    // duplicate the default rules.
    char resolved[INTEGR_NAME_MAX + 1];
    integrations_get_device_name(resolved, sizeof(resolved));
    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", true);
    cJSON_AddStringToObject(r, "device_name", resolved);
    cJSON_AddStringToObject(r, "hostname", resolved);
    cJSON_AddStringToObject(r, "ap_ssid", w.ap_ssid);
    return web_send_json(req, r);
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
    cJSON_AddStringToObject(r, "other_days",
                            sc.other_days == SCHED_OTHER_RUN ? "run" : "off");
    cJSON_AddNumberToObject(r, "utc_offset", sc.utc_offset);
    cJSON_AddStringToObject(r, "tz", sc.tz);
    // What the zone name actually resolved to, so a client can tell a
    // recognised zone from a typo that silently behaves like UTC.
    const char *posix = tz_posix_for_zone(sc.tz);
    cJSON_AddBoolToObject(r, "tz_known", posix != NULL);
    cJSON_AddStringToObject(r, "tz_posix", posix ? posix : "");
    cJSON_AddStringToObject(r, "off_action",
                            sc.off_action == SCHED_OFF_ACT_DIM ? "dim" : "off");
    cJSON_AddNumberToObject(r, "off_brightness", sc.off_brightness);
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
    const cJSON *tz = cJSON_GetObjectItem(json, "tz");
    const cJSON *oa = cJSON_GetObjectItem(json, "off_action");
    const cJSON *ob = cJSON_GetObjectItem(json, "off_brightness");
    const cJSON *od = cJSON_GetObjectItem(json, "other_days");

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
    // Absent off_action/off_brightness keep the stored values, so a client
    // that only knows the pre-2.2 fields cannot silently change the action.
    if (cJSON_IsNumber(ob) && (ob->valueint < 1 || ob->valueint > 255)) {
        cJSON_Delete(json);
        return web_send_err(req, "off_brightness 1-255", 400);
    }
    if (cJSON_IsString(oa)) {
        if      (!strcmp(oa->valuestring, "off")) sc.off_action = SCHED_OFF_ACT_OFF;
        else if (!strcmp(oa->valuestring, "dim")) sc.off_action = SCHED_OFF_ACT_DIM;
        else {
            cJSON_Delete(json);
            return web_send_err(req, "off_action must be off or dim", 400);
        }
    }
    if (cJSON_IsNumber(ob)) sc.off_brightness = (uint8_t)ob->valueint;
    if (cJSON_IsString(od)) {
        if      (!strcmp(od->valuestring, "off")) sc.other_days = SCHED_OTHER_OFF;
        else if (!strcmp(od->valuestring, "run")) sc.other_days = SCHED_OTHER_RUN;
        else {
            cJSON_Delete(json);
            return web_send_err(req, "other_days must be off or run", 400);
        }
    }

    // IANA zone name; "" clears it (back to the fixed utc_offset). A raw
    // POSIX rule is still accepted for anything the table does not carry, so
    // the check here is only length and charset.
    if (cJSON_IsString(tz)) {
        if (strlen(tz->valuestring) > SCHED_TZ_MAX) {
            cJSON_Delete(json);
            return web_send_err(req, "tz too long (max 48)", 400);
        }
        for (const char *p = tz->valuestring; *p; p++) {
            if (*p < 0x20 || *p > 0x7E) {
                cJSON_Delete(json);
                return web_send_err(req,
                                    "tz must be a printable IANA zone name, "
                                    "e.g. Europe/Zurich", 400);
            }
        }
        snprintf(sc.tz, sizeof(sc.tz), "%s", tz->valuestring);
    }

    if (cJSON_IsBool(en))   sc.enabled    = cJSON_IsTrue(en);
    if (cJSON_IsNumber(oh)) sc.on_hour    = (uint8_t)oh->valueint;
    if (cJSON_IsNumber(om)) sc.on_min     = (uint8_t)om->valueint;
    if (cJSON_IsNumber(fh)) sc.off_hour   = (uint8_t)fh->valueint;
    if (cJSON_IsNumber(fm)) sc.off_min    = (uint8_t)fm->valueint;
    if (cJSON_IsNumber(d))  sc.days       = (uint8_t)d->valueint & SCHED_DAYS_ALL;
    if (cJSON_IsNumber(u))  sc.utc_offset = (int16_t)u->valueint;

    cJSON_Delete(json);
    // scheduler_set() applies the schedule before it persists it, so a failed
    // write leaves the new schedule live until the next reboot - say so instead
    // of replying ok and silently reverting.
    esp_err_t err = scheduler_set(&sc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving schedule failed: %s", esp_err_to_name(err));
        return web_send_err(req, "schedule applied but not saved", 500);
    }
    return web_send_ok(req);
}

void api_system_register(httpd_handle_t srv)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/status",               .method = HTTP_GET,  .handler = status_get},
        {.uri = "/api/system/reboot",        .method = HTTP_POST, .handler = system_reboot_post},
        {.uri = "/api/system/boot_screen",   .method = HTTP_POST, .handler = system_boot_screen_post},
        {.uri = "/api/system/screens",       .method = HTTP_POST, .handler = system_screens_post},
        {.uri = "/api/system/device_name",   .method = HTTP_POST, .handler = system_device_name_post},
        {.uri = "/api/system/factory_reset", .method = HTTP_POST, .handler = system_factory_reset_post},
        {.uri = "/api/schedule",             .method = HTTP_GET,  .handler = schedule_get},
        {.uri = "/api/schedule",             .method = HTTP_POST, .handler = schedule_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        web_register_uri(srv, &uris[i]);
    }
}
