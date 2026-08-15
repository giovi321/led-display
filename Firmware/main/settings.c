#include "settings.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NS  = "cfg";

#define K_TEXT       "text"
#define K_COLOR_R    "cr"
#define K_COLOR_G    "cg"
#define K_COLOR_B    "cb"
#define K_BRIGHTNESS "bri"
#define K_BRI_CAP    "bcap"
#define K_SCROLL     "scrl"   // legacy on/off bool; kept in sync for rollback
#define K_SCROLL_MODE "scm"   // display_scroll_t; wins over K_SCROLL
#define K_SPEED      "spd"
#define K_SCALE      "scl"
#define K_FONT       "font"   // FONT_ID_*
#define K_PROP       "prop"   // proportional spacing bool
#define K_MAP_MODE   "map"    // legacy v1 key, read-only (migration)
#define K_BOOT_SCR   "boot"
#define K_SOURCE     "dsrc"   // display_source_t; absent = static
#define K_STARTUP    "scrs"   // startup_screen_t; absent = border splash
#define K_OFFLINE    "scro"   // offline_screen_t; absent = keep content
#define K_OFFLINE_TX "scrot"  // text for OFFLINE_TEXT
#define K_OFF_COL_R  "ocr"    // OFFLINE_DOT colour
#define K_OFF_COL_G  "ocg"
#define K_OFF_COL_B  "ocb"
#define K_OFF_CORNER "ocn"
#define K_DEVNAME    "dname"  // device name; absent = unset (defaults apply)

static void apply_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->text[0]         = '\0';
    s->color           = (display_color_t){80, 32, 0};   // matches display.c default
    s->brightness      = 32;                             // LED_DEFAULT_BRIGHTNESS
    s->brightness_cap  = 255;                            // uncapped
    s->scroll_mode     = 0;      // DISPLAY_SCROLL_OFF
    s->speed           = 50;
    s->scale           = 0;      // 0 -> display module picks a default
    s->font            = 0;      // FONT_ID_5X7, the pre-font-setting default
    s->prop            = true;
    s->boot_screen     = BOOT_SHOW_TEXT;
    s->source          = DISPLAY_SRC_STATIC;
    s->startup_screen  = STARTUP_BORDER;
    s->offline_screen  = OFFLINE_KEEP;
    s->offline_text[0] = '\0';
    s->offline_color   = (display_color_t){80, 0, 0};   // dim red
    s->offline_corner  = 0;
    s->legacy_map_mode = -1;     // absent
}

esp_err_t settings_load(settings_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    apply_defaults(out);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no stored settings yet, using defaults");
        return ESP_OK;
    }

    size_t len = sizeof(out->text);
    nvs_get_str(h, K_TEXT, out->text, &len);

    nvs_get_u8(h, K_COLOR_R,    &out->color.r);
    nvs_get_u8(h, K_COLOR_G,    &out->color.g);
    nvs_get_u8(h, K_COLOR_B,    &out->color.b);
    nvs_get_u8(h, K_BRIGHTNESS, &out->brightness);
    nvs_get_u8(h, K_BRI_CAP,    &out->brightness_cap);

    uint8_t v;
    // Two keys describe one setting: the pre-2.2 on/off bool and the full
    // off/restart/continuous mode. Every save from this build writes both, so a
    // disagreement means only the bool was written afterwards - by an older
    // image - and the bool then carries the newer choice. Cases: agree -> mode
    // wins (continuous survives); scm=2/scrl=0 -> off; scm=0/scrl=1 -> restart;
    // either key absent -> the other one.
    bool scrl_present = (nvs_get_u8(h, K_SCROLL, &v) == ESP_OK);
    bool scrl_on      = scrl_present && v != 0;
    if (scrl_present) out->scroll_mode = scrl_on ? 1 : 0;

    uint8_t scm;
    if (nvs_get_u8(h, K_SCROLL_MODE, &scm) == ESP_OK && scm <= 2 &&
        (!scrl_present || (scm != 0) == scrl_on)) {
        out->scroll_mode = scm;
    }
    nvs_get_u8(h, K_SPEED, &out->speed);
    nvs_get_u8(h, K_SCALE, &out->scale);
    nvs_get_u8(h, K_FONT, &out->font);
    if (nvs_get_u8(h, K_PROP, &v) == ESP_OK) out->prop = v ? true : false;
    if (nvs_get_u8(h, K_MAP_MODE, &v) == ESP_OK) out->legacy_map_mode = (int)v;
    if (nvs_get_u8(h, K_BOOT_SCR, &v) == ESP_OK && v <= BOOT_SHOW_IP_THEN) {
        out->boot_screen = (boot_screen_t)v;
    }
    if (nvs_get_u8(h, K_SOURCE, &v) == ESP_OK && v <= DISPLAY_SRC_INTEGRATION) {
        out->source = (display_source_t)v;
    }
    if (nvs_get_u8(h, K_STARTUP, &v) == ESP_OK && v <= STARTUP_TEXT) {
        out->startup_screen = (startup_screen_t)v;
    }
    if (nvs_get_u8(h, K_OFFLINE, &v) == ESP_OK && v <= OFFLINE_DOT) {
        out->offline_screen = (offline_screen_t)v;
    }
    len = sizeof(out->offline_text);
    if (nvs_get_str(h, K_OFFLINE_TX, out->offline_text, &len) != ESP_OK) {
        out->offline_text[0] = '\0';
    }
    nvs_get_u8(h, K_OFF_COL_R, &out->offline_color.r);
    nvs_get_u8(h, K_OFF_COL_G, &out->offline_color.g);
    nvs_get_u8(h, K_OFF_COL_B, &out->offline_color.b);
    if (nvs_get_u8(h, K_OFF_CORNER, &v) == ESP_OK && v <= 3) out->offline_corner = v;

    nvs_close(h);
    ESP_LOGI(TAG, "settings loaded: brightness=%u cap=%u scroll=%u speed=%u scale=%u font=%u prop=%d boot=%u text='%s'",
             out->brightness, out->brightness_cap, out->scroll_mode, out->speed,
             out->scale, out->font, out->prop, (unsigned)out->boot_screen, out->text);
    return ESP_OK;
}

esp_err_t settings_save(const settings_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_str(h, K_TEXT,       in->text);
    nvs_set_u8 (h, K_COLOR_R,    in->color.r);
    nvs_set_u8 (h, K_COLOR_G,    in->color.g);
    nvs_set_u8 (h, K_COLOR_B,    in->color.b);
    nvs_set_u8 (h, K_BRIGHTNESS, in->brightness);
    nvs_set_u8 (h, K_BRI_CAP,    in->brightness_cap);
    // Both scroll keys are written so an OTA rollback to a scroll-bool
    // build still sees a sensible on/off value.
    nvs_set_u8 (h, K_SCROLL,     in->scroll_mode ? 1 : 0);
    nvs_set_u8 (h, K_SCROLL_MODE, in->scroll_mode);
    nvs_set_u8 (h, K_SPEED,      in->speed);
    nvs_set_u8 (h, K_SCALE,      in->scale);
    nvs_set_u8 (h, K_FONT,       in->font);
    nvs_set_u8 (h, K_PROP,       in->prop ? 1 : 0);
    nvs_set_u8 (h, K_BOOT_SCR,   (uint8_t)in->boot_screen);
    nvs_set_u8 (h, K_SOURCE,     (uint8_t)in->source);
    nvs_set_u8 (h, K_STARTUP,    (uint8_t)in->startup_screen);
    nvs_set_u8 (h, K_OFFLINE,    (uint8_t)in->offline_screen);
    nvs_set_str(h, K_OFFLINE_TX, in->offline_text);
    nvs_set_u8 (h, K_OFF_COL_R,  in->offline_color.r);
    nvs_set_u8 (h, K_OFF_COL_G,  in->offline_color.g);
    nvs_set_u8 (h, K_OFF_COL_B,  in->offline_color.b);
    nvs_set_u8 (h, K_OFF_CORNER, in->offline_corner);

    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_save_text(const char *text,
                             const display_color_t *color,
                             uint8_t scroll_mode,
                             uint8_t speed,
                             uint8_t scale)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (text)  nvs_set_str(h, K_TEXT, text);
    if (color) {
        nvs_set_u8(h, K_COLOR_R, color->r);
        nvs_set_u8(h, K_COLOR_G, color->g);
        nvs_set_u8(h, K_COLOR_B, color->b);
    }
    nvs_set_u8(h, K_SCROLL, scroll_mode ? 1 : 0);
    nvs_set_u8(h, K_SCROLL_MODE, scroll_mode);
    nvs_set_u8(h, K_SPEED,  speed);
    if (scale != 0) nvs_set_u8(h, K_SCALE, scale);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_save_font(uint8_t font, bool prop)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_u8(h, K_FONT, font);
    nvs_set_u8(h, K_PROP, prop ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t save_u8(const char *key, uint8_t v)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // The set can fail on its own (a full or worn partition), so it has to be
    // reported: the callers turn this into the API's 500.
    err = nvs_set_u8(h, key, v);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_save_brightness(uint8_t brightness)
{
    return save_u8(K_BRIGHTNESS, brightness);
}

esp_err_t settings_save_brightness_cap(uint8_t cap)
{
    return save_u8(K_BRI_CAP, cap);
}

esp_err_t settings_save_boot_screen(boot_screen_t which)
{
    return save_u8(K_BOOT_SCR, (uint8_t)which);
}

esp_err_t settings_save_source(display_source_t source)
{
    return save_u8(K_SOURCE, (uint8_t)source);
}

esp_err_t settings_save_screens(const settings_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, K_STARTUP, (uint8_t)in->startup_screen);
    if (err == ESP_OK) err = nvs_set_u8(h, K_OFFLINE, (uint8_t)in->offline_screen);
    if (err == ESP_OK) err = nvs_set_str(h, K_OFFLINE_TX, in->offline_text);
    if (err == ESP_OK) err = nvs_set_u8(h, K_OFF_COL_R, in->offline_color.r);
    if (err == ESP_OK) err = nvs_set_u8(h, K_OFF_COL_G, in->offline_color.g);
    if (err == ESP_OK) err = nvs_set_u8(h, K_OFF_COL_B, in->offline_color.b);
    if (err == ESP_OK) err = nvs_set_u8(h, K_OFF_CORNER, in->offline_corner);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_clear_text(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_set_str(h, K_TEXT, "");
    nvs_set_u8(h, K_SCROLL, 0);
    nvs_set_u8(h, K_SCROLL_MODE, 0);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---- device name ----------------------------------------------------------

esp_err_t settings_load_device_name(char *buf, size_t bufsize)
{
    if (!buf || bufsize == 0) return ESP_ERR_INVALID_ARG;
    buf[0] = '\0';

    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return ESP_OK;   // unset
    size_t len = bufsize;
    if (nvs_get_str(h, K_DEVNAME, buf, &len) != ESP_OK) buf[0] = '\0';
    nvs_close(h);
    return ESP_OK;
}

esp_err_t settings_save_device_name(const char *name)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    if (name && name[0] != '\0') {
        err = nvs_set_str(h, K_DEVNAME, name);
    } else {
        err = nvs_erase_key(h, K_DEVNAME);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;   // already unset
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---- structured blobs ----------------------------------------------------

esp_err_t settings_load_blob(const char *key, char *buf, size_t bufsize,
                             size_t *out_len)
{
    if (!key || !buf || bufsize == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return ESP_ERR_NVS_NOT_FOUND;

    size_t len = 0;
    err = nvs_get_str(h, key, NULL, &len);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    if (len > bufsize) {
        nvs_close(h);
        return ESP_ERR_INVALID_SIZE;
    }
    err = nvs_get_str(h, key, buf, &len);
    nvs_close(h);
    if (err == ESP_OK && out_len) *out_len = len - 1;  // exclude the NUL
    return err;
}

esp_err_t settings_save_blob(const char *key, const char *json)
{
    if (!key || !json) return ESP_ERR_INVALID_ARG;
    if (strlen(json) >= SETTINGS_BLOB_MAX) return ESP_ERR_INVALID_SIZE;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, key, json);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t settings_erase_blob(const char *key)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(h, key);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---- layout blob <-> mapping_config_t -------------------------------------
//
// JSON shape (see DESIGN.md section 5):
// {"v":1,
//  "panel":{"w":32,"h":8,"origin":"TL","axis":"col","serpentine":true},
//  "grid":{"rows":2,"cols":2},
//  "cells":[{"row":0,"col":0,"chain":0,"rot":0}, ...]}

static const char *origin_names[] = {"TL", "TR", "BL", "BR"};
static const char *axis_names[]   = {"row", "col"};

esp_err_t settings_save_layout(const mapping_config_t *in)
{
    if (!in || mapping_validate(in) != MAPPING_OK) return ESP_ERR_INVALID_ARG;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "v", 1);

    cJSON *p = cJSON_AddObjectToObject(root, "panel");
    cJSON_AddNumberToObject(p, "w", in->panel.w);
    cJSON_AddNumberToObject(p, "h", in->panel.h);
    cJSON_AddStringToObject(p, "origin", origin_names[in->panel.origin]);
    cJSON_AddStringToObject(p, "axis", axis_names[in->panel.axis]);
    cJSON_AddBoolToObject(p, "serpentine", in->panel.serpentine);

    cJSON *g = cJSON_AddObjectToObject(root, "grid");
    cJSON_AddNumberToObject(g, "rows", in->rows);
    cJSON_AddNumberToObject(g, "cols", in->cols);

    cJSON *cells = cJSON_AddArrayToObject(root, "cells");
    for (int r = 0; r < in->rows; r++) {
        for (int c = 0; c < in->cols; c++) {
            cJSON *cell = cJSON_CreateObject();
            cJSON_AddNumberToObject(cell, "row", r);
            cJSON_AddNumberToObject(cell, "col", c);
            cJSON_AddNumberToObject(cell, "chain", in->cells[r][c].chain);
            cJSON_AddNumberToObject(cell, "rot", in->cells[r][c].rot);
            cJSON_AddItemToArray(cells, cell);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    esp_err_t err = settings_save_blob("layout", json);
    cJSON_free(json);
    return err;
}

esp_err_t settings_load_layout(mapping_config_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    char buf[SETTINGS_BLOB_MAX];
    esp_err_t err = settings_load_blob("layout", buf, sizeof(buf), NULL);
    if (err != ESP_OK) return err;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return ESP_ERR_INVALID_STATE;

    memset(out, 0, sizeof(*out));

    const cJSON *p = cJSON_GetObjectItem(root, "panel");
    const cJSON *g = cJSON_GetObjectItem(root, "grid");
    const cJSON *cells = cJSON_GetObjectItem(root, "cells");
    if (!cJSON_IsObject(p) || !cJSON_IsObject(g) || !cJSON_IsArray(cells)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_STATE;
    }

    const cJSON *w = cJSON_GetObjectItem(p, "w");
    const cJSON *h = cJSON_GetObjectItem(p, "h");
    const cJSON *o = cJSON_GetObjectItem(p, "origin");
    const cJSON *a = cJSON_GetObjectItem(p, "axis");
    const cJSON *s = cJSON_GetObjectItem(p, "serpentine");
    out->panel.w = cJSON_IsNumber(w) ? (uint8_t)w->valueint : 0;
    out->panel.h = cJSON_IsNumber(h) ? (uint8_t)h->valueint : 0;
    out->panel.serpentine = cJSON_IsBool(s) ? cJSON_IsTrue(s) : true;
    out->panel.origin = MAP_ORIGIN_TL;
    if (cJSON_IsString(o)) {
        for (int i = 0; i < 4; i++) {
            if (!strcmp(o->valuestring, origin_names[i])) out->panel.origin = (map_origin_t)i;
        }
    }
    out->panel.axis = MAP_AXIS_COL;
    if (cJSON_IsString(a) && !strcmp(a->valuestring, "row")) out->panel.axis = MAP_AXIS_ROW;

    const cJSON *rows = cJSON_GetObjectItem(g, "rows");
    const cJSON *cols = cJSON_GetObjectItem(g, "cols");
    out->rows = cJSON_IsNumber(rows) ? (uint8_t)rows->valueint : 0;
    out->cols = cJSON_IsNumber(cols) ? (uint8_t)cols->valueint : 0;

    const cJSON *cell;
    cJSON_ArrayForEach(cell, cells) {
        const cJSON *r  = cJSON_GetObjectItem(cell, "row");
        const cJSON *c  = cJSON_GetObjectItem(cell, "col");
        const cJSON *ch = cJSON_GetObjectItem(cell, "chain");
        const cJSON *ro = cJSON_GetObjectItem(cell, "rot");
        if (!cJSON_IsNumber(r) || !cJSON_IsNumber(c)) continue;
        int ri = r->valueint, ci = c->valueint;
        if (ri < 0 || ri >= MAP_MAX_ROWS || ci < 0 || ci >= MAP_MAX_COLS) continue;
        out->cells[ri][ci].chain = cJSON_IsNumber(ch) ? (uint8_t)ch->valueint : 0;
        out->cells[ri][ci].rot   = cJSON_IsNumber(ro) ? (uint16_t)ro->valueint : 0;
    }
    cJSON_Delete(root);

    if (mapping_validate(out) != MAPPING_OK) {
        ESP_LOGW(TAG, "stored layout blob failed validation, ignoring");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t settings_erase(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_all(h);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
