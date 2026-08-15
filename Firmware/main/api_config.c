// /api/config/export and /api/config/import (see api_handlers.h for the
// ownership table, DESIGN.md section 5 for shapes).
//
// The export is ONE JSON document holding every persisted setting except
// the WiFi credentials (deliberately: a config file that moves between
// devices or people should not carry the network password). It does contain
// the API key and the WireGuard private key, which is why the export
// endpoint requires auth even though it is a GET - web_require_auth()
// passes automatically while no key is configured.

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "api_handlers.h"
#include "display.h"
#include "fw_version.h"
#include "integrations.h"
#include "led_driver.h"
#include "light_sensor.h"
#include "mapping.h"
#include "mqtt_link.h"
#include "scheduler.h"
#include "settings.h"
#include "web_common.h"
#include "wg_client.h"

static const char *TAG = "api_config";

#define IMPORT_BODY_MAX  16384
#define COLORS_MAX       12
#define COLOR_NAME_MAX   24

// Mirrors the tables in integrations.c / api_colors.c; each file keeps its
// own copy because the frozen headers expose no name strings.
static const char *STALE_NAMES[] = {"none", "dim", "marker", "text"};
static const char *ORIGIN_NAMES[] = {"TL", "TR", "BL", "BR"};
static const char *AXIS_NAMES[]   = {"row", "col"};
static const char *SCROLL_NAMES[]  = {"off", "restart", "continuous"};
static const char *OFF_ACT_NAMES[] = {"off", "dim"};

// ---- section builders (export) ---------------------------------------------

static cJSON *display_to_json(const settings_t *s)
{
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "text", s->text);
    cJSON *col = cJSON_AddObjectToObject(d, "color");
    cJSON_AddNumberToObject(col, "r", s->color.r);
    cJSON_AddNumberToObject(col, "g", s->color.g);
    cJSON_AddNumberToObject(col, "b", s->color.b);
    cJSON_AddNumberToObject(d, "brightness", s->brightness);
    cJSON_AddNumberToObject(d, "brightness_cap", s->brightness_cap);
    // The bool keeps old backups/firmware working; the mode string is the
    // full off/restart/continuous state.
    cJSON_AddBoolToObject(d, "scroll", s->scroll_mode != 0);
    cJSON_AddStringToObject(d, "scroll_mode",
                            SCROLL_NAMES[s->scroll_mode <= 2 ? s->scroll_mode : 1]);
    cJSON_AddNumberToObject(d, "speed", s->speed);
    cJSON_AddNumberToObject(d, "scale", s->scale);
    cJSON_AddStringToObject(d, "font", font_name(s->font));
    cJSON_AddStringToObject(d, "spacing", s->prop ? "proportional" : "monospace");
    cJSON_AddNumberToObject(d, "boot_screen", (int)s->boot_screen);
    cJSON_AddStringToObject(d, "source",
                            s->source == DISPLAY_SRC_INTEGRATION ? "integration"
                                                                 : "static");
    cJSON_AddNumberToObject(d, "startup_screen", (int)s->startup_screen);
    cJSON_AddNumberToObject(d, "offline_screen", (int)s->offline_screen);
    cJSON_AddStringToObject(d, "offline_text", s->offline_text);
    cJSON *ocol = cJSON_AddObjectToObject(d, "offline_color");
    cJSON_AddNumberToObject(ocol, "r", s->offline_color.r);
    cJSON_AddNumberToObject(ocol, "g", s->offline_color.g);
    cJSON_AddNumberToObject(ocol, "b", s->offline_color.b);
    cJSON_AddNumberToObject(d, "offline_x", s->offline_x);
    cJSON_AddNumberToObject(d, "offline_y", s->offline_y);
    cJSON_AddNumberToObject(d, "offline_brightness", s->offline_bright);
    return d;
}

// Same shape as the "layout" blob written by settings_save_layout(), built
// from the ACTIVE driver config so the export reflects reality even before
// the user ever saved a layout.
static cJSON *layout_to_json(const mapping_config_t *m)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);

    cJSON *p = cJSON_AddObjectToObject(root, "panel");
    cJSON_AddNumberToObject(p, "w", m->panel.w);
    cJSON_AddNumberToObject(p, "h", m->panel.h);
    cJSON_AddStringToObject(p, "origin", ORIGIN_NAMES[m->panel.origin]);
    cJSON_AddStringToObject(p, "axis", AXIS_NAMES[m->panel.axis]);
    cJSON_AddBoolToObject(p, "serpentine", m->panel.serpentine);

    cJSON *g = cJSON_AddObjectToObject(root, "grid");
    cJSON_AddNumberToObject(g, "rows", m->rows);
    cJSON_AddNumberToObject(g, "cols", m->cols);

    cJSON *cells = cJSON_AddArrayToObject(root, "cells");
    for (int r = 0; r < m->rows; r++) {
        for (int c = 0; c < m->cols; c++) {
            cJSON *cell = cJSON_CreateObject();
            cJSON_AddNumberToObject(cell, "row", r);
            cJSON_AddNumberToObject(cell, "col", c);
            cJSON_AddNumberToObject(cell, "chain", m->cells[r][c].chain);
            cJSON_AddNumberToObject(cell, "rot", m->cells[r][c].rot);
            cJSON_AddItemToArray(cells, cell);
        }
    }
    return root;
}

// Stored colours blob parsed back to an object; empty canonical shape when
// nothing was saved yet.
static cJSON *colors_to_json(void)
{
    cJSON *parsed = NULL;
    char *buf = malloc(SETTINGS_BLOB_MAX);
    if (buf && settings_load_blob("colors", buf, SETTINGS_BLOB_MAX, NULL) == ESP_OK) {
        parsed = cJSON_Parse(buf);
    }
    free(buf);
    if (parsed) return parsed;

    cJSON *empty = cJSON_CreateObject();
    cJSON_AddNumberToObject(empty, "v", 1);
    cJSON_AddArrayToObject(empty, "colors");
    return empty;
}

static cJSON *value_cfg_to_json(void)
{
    value_cfg_t v;
    integrations_get_value_cfg(&v);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "v", 1);
    cJSON_AddStringToObject(r, "prefix", v.prefix);
    cJSON_AddStringToObject(r, "suffix", v.suffix);
    cJSON_AddNumberToObject(r, "decimals", v.decimals);
    char thou[2] = {v.thousands, '\0'};
    cJSON_AddStringToObject(r, "thousands", thou);
    cJSON_AddNumberToObject(r, "stale_hours", v.stale_hours);
    unsigned act = (unsigned)v.stale_action;
    if (act >= sizeof(STALE_NAMES) / sizeof(STALE_NAMES[0])) act = 0;
    cJSON_AddStringToObject(r, "stale_action", STALE_NAMES[act]);
    cJSON_AddStringToObject(r, "stale_text", v.stale_text);
    cJSON_AddStringToObject(r, "default_text", v.default_text);
    return r;
}

static cJSON *integrations_to_json(void)
{
    integrations_cfg_t c;
    integrations_get(&c);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "v", 1);
    cJSON_AddStringToObject(r, "api_key", c.api_key);   // secret: export is auth-gated
    cJSON_AddStringToObject(r, "webhook_url", c.webhook_url);
    cJSON_AddNumberToObject(r, "webhook_events", c.webhook_events);
    cJSON_AddStringToObject(r, "device_name", c.device_name);
    cJSON_AddBoolToObject(r, "ui_auth", c.ui_auth);
    cJSON_AddStringToObject(r, "ui_user", c.ui_user);
    cJSON_AddStringToObject(r, "ui_pass", c.ui_pass);   // secret: export is auth-gated
    return r;
}

static cJSON *wireguard_to_json(void)
{
    wg_config_t c;
    wg_client_get_config(&c);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "enabled", c.enabled);
    cJSON_AddStringToObject(r, "private_key", c.private_key);   // secret: auth-gated
    cJSON_AddStringToObject(r, "peer_public_key", c.peer_public_key);
    cJSON_AddStringToObject(r, "endpoint_host", c.endpoint_host);
    cJSON_AddNumberToObject(r, "endpoint_port", c.endpoint_port);
    cJSON_AddStringToObject(r, "address", c.address);
    cJSON_AddStringToObject(r, "allowed_ips", c.allowed_ips);
    cJSON_AddNumberToObject(r, "keepalive_s", c.keepalive_s);
    return r;
}

static cJSON *mqtt_to_json(void)
{
    mqtt_cfg_t c;
    mqtt_link_get_config(&c);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "v", 1);
    cJSON_AddBoolToObject(r, "enabled", c.enabled);
    cJSON_AddStringToObject(r, "uri", c.uri);
    cJSON_AddStringToObject(r, "username", c.username);
    cJSON_AddStringToObject(r, "password", c.password);   // secret: auth-gated
    cJSON_AddStringToObject(r, "base_topic", c.base_topic);
    cJSON_AddBoolToObject(r, "ha_discovery", c.ha_discovery);
    cJSON_AddStringToObject(r, "ha_prefix", c.ha_prefix);
    return r;
}

// Same field names as GET /api/display/light_sensor, minus the live reading:
// a backup describes the configuration, not what the room looked like.
static cJSON *light_sensor_to_json(void)
{
    light_sensor_cfg_t c;
    light_sensor_get(&c);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "v", 1);
    cJSON_AddBoolToObject(r, "enabled", c.enabled);
    cJSON_AddNumberToObject(r, "gpio", c.gpio);
    cJSON_AddNumberToObject(r, "dark_raw", c.curve.dark_raw);
    cJSON_AddNumberToObject(r, "bright_raw", c.curve.bright_raw);
    cJSON_AddNumberToObject(r, "min_brightness", c.curve.min_bri);
    cJSON_AddNumberToObject(r, "max_brightness", c.curve.max_bri);
    cJSON_AddNumberToObject(r, "gamma_x10", c.curve.gamma_x10);
    return r;
}

static cJSON *schedule_to_json(void)
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
    cJSON_AddStringToObject(r, "off_action", OFF_ACT_NAMES[sc.off_action <= 1 ? sc.off_action : 0]);
    cJSON_AddNumberToObject(r, "off_brightness", sc.off_brightness);
    return r;
}

// ---- GET /api/config/export ------------------------------------------------

static esp_err_t config_export(httpd_req_t *req)
{
    // ALWAYS auth-checked: this document contains the API key and the
    // WireGuard private key. With no key configured the check passes.
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    settings_t s;
    settings_load(&s);
    mapping_config_t map;
    led_driver_get_config(&map);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "fw", FW_VERSION);
    cJSON_AddStringToObject(root, "note", "wifi credentials not included");
    cJSON_AddItemToObject(root, "display", display_to_json(&s));
    cJSON_AddItemToObject(root, "layout", layout_to_json(&map));
    cJSON_AddItemToObject(root, "colors", colors_to_json());
    cJSON_AddItemToObject(root, "value_config", value_cfg_to_json());
    cJSON_AddItemToObject(root, "integrations", integrations_to_json());
    cJSON_AddItemToObject(root, "wireguard", wireguard_to_json());
    cJSON_AddItemToObject(root, "mqtt", mqtt_to_json());
    cJSON_AddItemToObject(root, "schedule", schedule_to_json());
    cJSON_AddItemToObject(root, "light_sensor", light_sensor_to_json());

    httpd_resp_set_hdr(req, "Content-Disposition",
                       "attachment; filename=led-display-config.json");
    return web_send_json(req, root);
}

// ---- import: per-section apply helpers --------------------------------------

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool apply_layout(const cJSON *sec)
{
    const cJSON *p     = cJSON_GetObjectItem(sec, "panel");
    const cJSON *g     = cJSON_GetObjectItem(sec, "grid");
    const cJSON *cells = cJSON_GetObjectItem(sec, "cells");
    if (!cJSON_IsObject(p) || !cJSON_IsObject(g) || !cJSON_IsArray(cells)) {
        return false;
    }

    mapping_config_t m;
    memset(&m, 0, sizeof(m));

    const cJSON *w = cJSON_GetObjectItem(p, "w");
    const cJSON *h = cJSON_GetObjectItem(p, "h");
    const cJSON *o = cJSON_GetObjectItem(p, "origin");
    const cJSON *a = cJSON_GetObjectItem(p, "axis");
    const cJSON *sp = cJSON_GetObjectItem(p, "serpentine");
    // Range-check before the narrow casts: (uint8_t)320 would wrap to 64 and
    // sail through mapping_validate as a plausible-but-wrong layout.
    if (!cJSON_IsNumber(w) || !cJSON_IsNumber(h) ||
        w->valueint < 1 || w->valueint > 64 ||
        h->valueint < 1 || h->valueint > 64) {
        return false;
    }
    m.panel.w = (uint8_t)w->valueint;
    m.panel.h = (uint8_t)h->valueint;
    m.panel.serpentine = cJSON_IsBool(sp) ? cJSON_IsTrue(sp) : true;
    m.panel.origin = MAP_ORIGIN_TL;
    if (cJSON_IsString(o)) {
        for (int i = 0; i < 4; i++) {
            if (strcmp(o->valuestring, ORIGIN_NAMES[i]) == 0) {
                m.panel.origin = (map_origin_t)i;
            }
        }
    }
    m.panel.axis = MAP_AXIS_COL;
    if (cJSON_IsString(a) && strcmp(a->valuestring, "row") == 0) {
        m.panel.axis = MAP_AXIS_ROW;
    }

    const cJSON *rows = cJSON_GetObjectItem(g, "rows");
    const cJSON *cols = cJSON_GetObjectItem(g, "cols");
    if (!cJSON_IsNumber(rows) || !cJSON_IsNumber(cols) ||
        rows->valueint < 1 || rows->valueint > MAP_MAX_ROWS ||
        cols->valueint < 1 || cols->valueint > MAP_MAX_COLS) {
        return false;
    }
    m.rows = (uint8_t)rows->valueint;
    m.cols = (uint8_t)cols->valueint;

    const cJSON *cell;
    cJSON_ArrayForEach(cell, cells) {
        const cJSON *r  = cJSON_GetObjectItem(cell, "row");
        const cJSON *c  = cJSON_GetObjectItem(cell, "col");
        const cJSON *ch = cJSON_GetObjectItem(cell, "chain");
        const cJSON *ro = cJSON_GetObjectItem(cell, "rot");
        if (!cJSON_IsNumber(r) || !cJSON_IsNumber(c)) continue;
        int ri = r->valueint, ci = c->valueint;
        if (ri < 0 || ri >= MAP_MAX_ROWS || ci < 0 || ci >= MAP_MAX_COLS) continue;
        int chain = cJSON_IsNumber(ch) ? ch->valueint : 0;
        int rot   = cJSON_IsNumber(ro) ? ro->valueint : 0;
        if (chain < 0 || chain >= MAP_MAX_PANELS || rot < 0 || rot > 270) {
            return false;   // would wrap through the narrow casts
        }
        m.cells[ri][ci].chain = (uint8_t)chain;
        m.cells[ri][ci].rot   = (uint16_t)rot;
    }

    if (mapping_validate(&m) != MAPPING_OK) {
        ESP_LOGW(TAG, "import: layout failed validation");
        return false;
    }
    if (led_driver_reconfigure(&m) != ESP_OK) {
        ESP_LOGW(TAG, "import: layout reconfigure failed");
        return false;
    }
    return settings_save_layout(&m) == ESP_OK;
}

static bool apply_display(const cJSON *sec)
{
    settings_t s;
    settings_load(&s);

    const cJSON *it;
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "text"))) {
        snprintf(s.text, sizeof(s.text), "%s", it->valuestring);
    }
    const cJSON *col = cJSON_GetObjectItem(sec, "color");
    if (cJSON_IsObject(col)) {
        const cJSON *r = cJSON_GetObjectItem(col, "r");
        const cJSON *g = cJSON_GetObjectItem(col, "g");
        const cJSON *b = cJSON_GetObjectItem(col, "b");
        if (cJSON_IsNumber(r)) s.color.r = (uint8_t)clampi(r->valueint, 0, 255);
        if (cJSON_IsNumber(g)) s.color.g = (uint8_t)clampi(g->valueint, 0, 255);
        if (cJSON_IsNumber(b)) s.color.b = (uint8_t)clampi(b->valueint, 0, 255);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "brightness"))) {
        s.brightness = (uint8_t)clampi(it->valueint, 0, 255);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "brightness_cap"))) {
        s.brightness_cap = (uint8_t)clampi(it->valueint, 0, 255);
    }
    if (cJSON_IsBool(it = cJSON_GetObjectItem(sec, "scroll"))) {
        // Pre-2.2 backups only carry the bool; mapped like the NVS key.
        s.scroll_mode = cJSON_IsTrue(it) ? 1 : 0;
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "scroll_mode"))) {
        for (unsigned i = 0; i < sizeof(SCROLL_NAMES) / sizeof(SCROLL_NAMES[0]); i++) {
            if (strcmp(it->valuestring, SCROLL_NAMES[i]) == 0) {
                s.scroll_mode = (uint8_t)i;
            }
        }
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "speed"))) {
        s.speed = (uint8_t)clampi(it->valueint, 0, 255);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "scale"))) {
        s.scale = (uint8_t)clampi(it->valueint, 0, FONT_SCALE_MAX);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "font"))) {
        int id = font_id_by_name(it->valuestring);
        if (id >= 0) s.font = (uint8_t)id;   // unknown names keep the current font
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "spacing"))) {
        if      (strcmp(it->valuestring, "proportional") == 0) s.prop = true;
        else if (strcmp(it->valuestring, "monospace") == 0)    s.prop = false;
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "boot_screen"))) {
        s.boot_screen = (boot_screen_t)clampi(it->valueint, 0, BOOT_SHOW_IP_THEN);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "source"))) {
        if      (strcmp(it->valuestring, "integration") == 0) s.source = DISPLAY_SRC_INTEGRATION;
        else if (strcmp(it->valuestring, "static") == 0)      s.source = DISPLAY_SRC_STATIC;
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "startup_screen"))) {
        s.startup_screen = (startup_screen_t)clampi(it->valueint, 0, STARTUP_TEXT);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "offline_screen"))) {
        s.offline_screen = (offline_screen_t)clampi(it->valueint, 0, OFFLINE_DOT);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "offline_text"))) {
        snprintf(s.offline_text, sizeof(s.offline_text), "%s", it->valuestring);
    }
    const cJSON *ocol = cJSON_GetObjectItem(sec, "offline_color");
    if (cJSON_IsObject(ocol)) {
        const cJSON *r = cJSON_GetObjectItem(ocol, "r");
        const cJSON *g = cJSON_GetObjectItem(ocol, "g");
        const cJSON *b = cJSON_GetObjectItem(ocol, "b");
        if (cJSON_IsNumber(r)) s.offline_color.r = (uint8_t)clampi(r->valueint, 0, 255);
        if (cJSON_IsNumber(g)) s.offline_color.g = (uint8_t)clampi(g->valueint, 0, 255);
        if (cJSON_IsNumber(b)) s.offline_color.b = (uint8_t)clampi(b->valueint, 0, 255);
    }
    // A backup taken before 2.7.0 carries offline_corner and no coordinates;
    // leaving x/y at OFFLINE_POS_UNSET hands it to the same one-shot
    // migration a stored corner goes through on the next boot.
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "offline_corner"))) {
        s.offline_corner = (uint8_t)clampi(it->valueint, 0, 3);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "offline_x"))) {
        s.offline_x = (uint16_t)clampi(it->valueint, 0, OFFLINE_POS_UNSET);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "offline_y"))) {
        s.offline_y = (uint16_t)clampi(it->valueint, 0, OFFLINE_POS_UNSET);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "offline_brightness"))) {
        s.offline_bright = (uint8_t)clampi(it->valueint, 0, 255);
    }

    if (settings_save(&s) != ESP_OK) return false;

    // Live-apply. The cap goes first so the brightness lands under it, then the
    // arbiter picks the level: a scheduled-off window keeps the panel dark (the
    // text still renders into the frame) and an enabled light sensor keeps
    // driving it, with the imported value stored for when it stops.
    led_driver_set_brightness_cap(s.brightness_cap);
    scheduler_reapply_brightness();
    display_set_font(s.font, s.prop);
    if (s.text[0] != '\0') {
        display_show_text(s.text, s.color, s.scroll_mode, s.speed, s.scale);
    } else {
        display_clear();
    }
    return true;
}

// Same validation as POST /api/colors (kept in sync by hand; the canonical
// blob shape is small enough that sharing code across handler files is not
// worth a new header).
static bool apply_colors(const cJSON *sec)
{
    const cJSON *colors = cJSON_IsArray(sec) ? sec
                        : cJSON_GetObjectItem(sec, "colors");
    if (!cJSON_IsArray(colors) || cJSON_GetArraySize(colors) > COLORS_MAX) {
        return false;
    }

    cJSON *blob = cJSON_CreateObject();
    cJSON_AddNumberToObject(blob, "v", 1);
    cJSON *out = cJSON_AddArrayToObject(blob, "colors");

    const cJSON *it;
    cJSON_ArrayForEach(it, colors) {
        const cJSON *name = cJSON_GetObjectItem(it, "name");
        const cJSON *r    = cJSON_GetObjectItem(it, "r");
        const cJSON *g    = cJSON_GetObjectItem(it, "g");
        const cJSON *b    = cJSON_GetObjectItem(it, "b");
        if (!cJSON_IsString(name) || name->valuestring[0] == '\0' ||
            strlen(name->valuestring) > COLOR_NAME_MAX ||
            !cJSON_IsNumber(r) || r->valueint < 0 || r->valueint > 255 ||
            !cJSON_IsNumber(g) || g->valueint < 0 || g->valueint > 255 ||
            !cJSON_IsNumber(b) || b->valueint < 0 || b->valueint > 255) {
            cJSON_Delete(blob);
            return false;
        }
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", name->valuestring);
        cJSON_AddNumberToObject(entry, "r", r->valueint);
        cJSON_AddNumberToObject(entry, "g", g->valueint);
        cJSON_AddNumberToObject(entry, "b", b->valueint);
        cJSON_AddItemToArray(out, entry);
    }

    char *json = cJSON_PrintUnformatted(blob);
    cJSON_Delete(blob);
    if (!json) return false;
    esp_err_t err = settings_save_blob("colors", json);
    cJSON_free(json);
    return err == ESP_OK;
}

static bool apply_value_config(const cJSON *sec)
{
    value_cfg_t v;
    integrations_get_value_cfg(&v);

    const cJSON *it;
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "prefix"))) {
        snprintf(v.prefix, sizeof(v.prefix), "%s", it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "suffix"))) {
        snprintf(v.suffix, sizeof(v.suffix), "%s", it->valuestring);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "decimals"))) {
        v.decimals = (int8_t)clampi(it->valueint, -1, 9);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "thousands"))) {
        v.thousands = it->valuestring[0];
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "stale_hours"))) {
        v.stale_hours = (uint16_t)clampi(it->valueint, 0, 0xFFFF);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "stale_action"))) {
        for (unsigned i = 0; i < sizeof(STALE_NAMES) / sizeof(STALE_NAMES[0]); i++) {
            if (strcmp(it->valuestring, STALE_NAMES[i]) == 0) {
                v.stale_action = (stale_action_t)i;
            }
        }
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "stale_text"))) {
        snprintf(v.stale_text, sizeof(v.stale_text), "%s", it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "default_text"))) {
        snprintf(v.default_text, sizeof(v.default_text), "%s", it->valuestring);
    }

    // integrations_set_value_cfg persists the blob AND refreshes the RAM
    // cache, which is the "re-init" the import contract asks for.
    return integrations_set_value_cfg(&v) == ESP_OK;
}

static bool apply_integrations(const cJSON *sec)
{
    integrations_cfg_t c;
    integrations_get(&c);

    const cJSON *it;
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "api_key"))) {
        snprintf(c.api_key, sizeof(c.api_key), "%s", it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "webhook_url"))) {
        snprintf(c.webhook_url, sizeof(c.webhook_url), "%s", it->valuestring);
    }
    // The device name is now a hostname and an SSID too. integrations_set()
    // cleans it to that charset, so a pre-2.2 backup carrying "Office Panel"
    // still imports (as "Office-Panel") instead of failing the section.
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "device_name"))) {
        snprintf(c.device_name, sizeof(c.device_name), "%s", it->valuestring);
    }
    // The export writes the mask as a number (blob shape); name arrays are
    // the /api/integrations dialect, not the config-file one.
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "webhook_events"))) {
        c.webhook_events = (uint32_t)it->valuedouble;
    }
    // Browser login. Absent in a pre-2.8.0 backup, which leaves the stored
    // setting alone; restoring one that enables it without carrying a
    // password would lock the UI with no way in, so that combination is
    // demoted to off rather than honoured.
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "ui_user"))) {
        snprintf(c.ui_user, sizeof(c.ui_user), "%s", it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "ui_pass"))) {
        snprintf(c.ui_pass, sizeof(c.ui_pass), "%s", it->valuestring);
    }
    if ((it = cJSON_GetObjectItem(sec, "ui_auth")) != NULL && cJSON_IsBool(it)) {
        c.ui_auth = cJSON_IsTrue(it) && c.ui_pass[0] != '\0';
    }
    return integrations_set(&c) == ESP_OK;
}

static bool apply_wireguard(const cJSON *sec)
{
    wg_config_t c;
    wg_client_get_config(&c);

    const cJSON *it;
    if (cJSON_IsBool(it = cJSON_GetObjectItem(sec, "enabled"))) {
        c.enabled = cJSON_IsTrue(it);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "private_key")) &&
        it->valuestring[0] != '\0') {
        snprintf(c.private_key, sizeof(c.private_key), "%s", it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "peer_public_key"))) {
        snprintf(c.peer_public_key, sizeof(c.peer_public_key), "%s", it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "endpoint_host"))) {
        snprintf(c.endpoint_host, sizeof(c.endpoint_host), "%s", it->valuestring);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "endpoint_port"))) {
        c.endpoint_port = (uint16_t)clampi(it->valueint, 0, 0xFFFF);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "address"))) {
        snprintf(c.address, sizeof(c.address), "%s", it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "allowed_ips"))) {
        snprintf(c.allowed_ips, sizeof(c.allowed_ips), "%s", it->valuestring);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "keepalive_s"))) {
        c.keepalive_s = (uint16_t)clampi(it->valueint, 0, 0xFFFF);
    }

    esp_err_t err = wg_client_apply(&c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "import: wg_client_apply failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static bool apply_mqtt(const cJSON *sec)
{
    mqtt_cfg_t c;
    mqtt_link_get_config(&c);

    const cJSON *it;
    if (cJSON_IsBool(it = cJSON_GetObjectItem(sec, "enabled"))) {
        c.enabled = cJSON_IsTrue(it);
    }
    if (cJSON_IsBool(it = cJSON_GetObjectItem(sec, "ha_discovery"))) {
        c.ha_discovery = cJSON_IsTrue(it);
    }
    struct { const char *key; char *dst; size_t sz; bool keep_empty; } strs[] = {
        {"uri",        c.uri,        sizeof(c.uri),        false},
        {"username",   c.username,   sizeof(c.username),   false},
        // An empty password in the file keeps the stored one (mirrors the
        // wireguard private_key rule above).
        {"password",   c.password,   sizeof(c.password),   true},
        {"base_topic", c.base_topic, sizeof(c.base_topic), false},
        {"ha_prefix",  c.ha_prefix,  sizeof(c.ha_prefix),  false},
    };
    for (unsigned i = 0; i < sizeof(strs) / sizeof(strs[0]); i++) {
        if (!cJSON_IsString(it = cJSON_GetObjectItem(sec, strs[i].key))) continue;
        if (strs[i].keep_empty && it->valuestring[0] == '\0') continue;
        snprintf(strs[i].dst, strs[i].sz, "%s", it->valuestring);
    }

    esp_err_t err = mqtt_link_apply(&c);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "import: mqtt_link_apply failed: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static bool apply_light_sensor(const cJSON *sec)
{
    light_sensor_cfg_t c;
    light_sensor_get(&c);

    const cJSON *it;
    if (cJSON_IsBool(it = cJSON_GetObjectItem(sec, "enabled"))) {
        c.enabled = cJSON_IsTrue(it);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "gpio"))) {
        // Validated, not clamped: clamping would silently move the sensor to
        // GPIO10, and a file naming a pin this board cannot read belongs in
        // "skipped".
        int g = it->valueint;
        if (g != 0 && !light_curve_valid_gpio(g)) return false;
        c.gpio = (uint8_t)g;
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "dark_raw"))) {
        c.curve.dark_raw = (uint16_t)clampi(it->valueint, 0, LIGHT_RAW_MAX);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "bright_raw"))) {
        c.curve.bright_raw = (uint16_t)clampi(it->valueint, 0, LIGHT_RAW_MAX);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "min_brightness"))) {
        c.curve.min_bri = (uint8_t)clampi(it->valueint, 0, 255);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "max_brightness"))) {
        c.curve.max_bri = (uint8_t)clampi(it->valueint, 0, 255);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "gamma_x10"))) {
        c.curve.gamma_x10 = (uint8_t)clampi(it->valueint, LIGHT_GAMMA_MIN,
                                            LIGHT_GAMMA_MAX);
    }
    // A file describing a sensor this board does not have (no pin, a pin it
    // cannot read, or two equal calibration points) is reported as skipped
    // rather than half-applied.
    return light_sensor_set(&c) == ESP_OK;
}

static bool apply_schedule(const cJSON *sec)
{
    schedule_t sc;
    scheduler_get(&sc);

    const cJSON *it;
    if (cJSON_IsBool(it = cJSON_GetObjectItem(sec, "enabled"))) {
        sc.enabled = cJSON_IsTrue(it);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "on_hour"))) {
        sc.on_hour = (uint8_t)clampi(it->valueint, 0, 23);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "on_min"))) {
        sc.on_min = (uint8_t)clampi(it->valueint, 0, 59);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "off_hour"))) {
        sc.off_hour = (uint8_t)clampi(it->valueint, 0, 23);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "off_min"))) {
        sc.off_min = (uint8_t)clampi(it->valueint, 0, 59);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "days"))) {
        sc.days = (uint8_t)(it->valueint & SCHED_DAYS_ALL);
    }
    // Absent in a pre-2.8.0 backup, where an unticked day always held the
    // off-action: leaving the stored value alone restores exactly that.
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "other_days"))) {
        if      (!strcmp(it->valuestring, "off")) sc.other_days = SCHED_OTHER_OFF;
        else if (!strcmp(it->valuestring, "run")) sc.other_days = SCHED_OTHER_RUN;
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "utc_offset"))) {
        sc.utc_offset = (int16_t)clampi(it->valueint, -14 * 60, 14 * 60);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "tz")) &&
        strlen(it->valuestring) <= SCHED_TZ_MAX) {
        snprintf(sc.tz, sizeof(sc.tz), "%s", it->valuestring);
    }
    // Pre-2.2 backups carry neither field: the stored action stays as it is,
    // which for a device that never set one is "off" (old behaviour).
    if (cJSON_IsString(it = cJSON_GetObjectItem(sec, "off_action"))) {
        for (unsigned i = 0; i < sizeof(OFF_ACT_NAMES) / sizeof(OFF_ACT_NAMES[0]); i++) {
            if (strcmp(it->valuestring, OFF_ACT_NAMES[i]) == 0) sc.off_action = (uint8_t)i;
        }
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(sec, "off_brightness"))) {
        sc.off_brightness = (uint8_t)clampi(it->valueint, 1, 255);
    }
    return scheduler_set(&sc) == ESP_OK;
}

// ---- POST /api/config/import -----------------------------------------------

static esp_err_t config_import(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *doc = web_read_json(req, IMPORT_BODY_MAX);
    if (!doc) return web_send_err(req, "invalid JSON body", 400);

    cJSON *applied = cJSON_CreateArray();
    cJSON *skipped = cJSON_CreateArray();

    typedef bool (*apply_fn_t)(const cJSON *);
    // Layout goes first so an imported display text renders onto the
    // imported geometry, not the old one.
    static const struct {
        const char *name;
        apply_fn_t  fn;
    } sections[] = {
        {"layout",       apply_layout},
        {"display",      apply_display},
        {"light_sensor", apply_light_sensor},
        {"colors",       apply_colors},
        {"value_config", apply_value_config},
        {"integrations", apply_integrations},
        {"wireguard",    apply_wireguard},
        {"mqtt",         apply_mqtt},
        {"schedule",     apply_schedule},
    };

    bool layout_applied  = false;
    bool display_present = false;

    for (size_t i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        const cJSON *sec = cJSON_GetObjectItem(doc, sections[i].name);
        if (!sec) continue;   // absent sections are simply not touched

        bool ok = sections[i].fn(sec);
        cJSON_AddItemToArray(ok ? applied : skipped,
                             cJSON_CreateString(sections[i].name));
        if (ok && strcmp(sections[i].name, "layout") == 0) layout_applied = true;
        if (strcmp(sections[i].name, "display") == 0) display_present = true;
        if (!ok) {
            ESP_LOGW(TAG, "import: section '%s' skipped", sections[i].name);
        }
    }

    // led_driver_reconfigure cleared the frame; if no display section came
    // along to re-render, bring the stored text back onto the new layout.
    if (layout_applied && !display_present) {
        settings_t s;
        settings_load(&s);
        if (s.text[0] != '\0') {
            display_show_text(s.text, s.color, s.scroll_mode, s.speed, s.scale);
        }
    }
    cJSON_Delete(doc);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddItemToObject(resp, "applied", applied);
    cJSON_AddItemToObject(resp, "skipped", skipped);
    return web_send_json(req, resp);
}

// ---- registration ----------------------------------------------------------

void api_config_register(httpd_handle_t srv)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/config/export", .method = HTTP_GET,  .handler = config_export},
        {.uri = "/api/config/import", .method = HTTP_POST, .handler = config_import},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        web_register_uri(srv, &uris[i]);
    }
}
