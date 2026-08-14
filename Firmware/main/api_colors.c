// /api/colors, /api/value and /api/value/config (see api_handlers.h for the
// ownership table, DESIGN.md section 5 for shapes).
//
// Saved colours are stored as one JSON blob ("colors" key,
// {"v":1,"colors":[{"name":"amber","r":80,"g":32,"b":0},...]}): POST
// replaces the whole list so the web UI never has to diff. /api/value is
// the generic inbound endpoint: whatever posts a number here (a webhook
// source, a home-automation flow, curl) gets it formatted through the
// value config and shown in the display's current colour, scrolling mode
// and speed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "api_handlers.h"
#include "display.h"
#include "integrations.h"
#include "settings.h"
#include "valuefmt.h"
#include "web_common.h"

static const char *TAG = "api_colors";

#define COLORS_BLOB_KEY  "colors"
#define COLORS_MAX       12
#define COLOR_NAME_MAX   24

// Mirrors the table in integrations.c; both files keep it private because
// the frozen headers give the enum no name strings.
static const char *STALE_NAMES[] = {"none", "dim", "marker", "text"};

// ---- /api/colors -----------------------------------------------------------

static esp_err_t colors_get(httpd_req_t *req)
{
    cJSON *arr = NULL;

    char *buf = malloc(SETTINGS_BLOB_MAX);
    if (buf && settings_load_blob(COLORS_BLOB_KEY, buf, SETTINGS_BLOB_MAX,
                                  NULL) == ESP_OK) {
        cJSON *blob = cJSON_Parse(buf);
        if (blob) {
            cJSON *colors = cJSON_DetachItemFromObject(blob, "colors");
            if (cJSON_IsArray(colors)) {
                arr = colors;
            } else {
                cJSON_Delete(colors);
            }
            cJSON_Delete(blob);
        }
    }
    free(buf);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "colors", arr ? arr : cJSON_CreateArray());
    return web_send_json(req, resp);
}

// Validate a client-supplied colour list and rebuild it as the canonical
// blob (dropping any extra fields). Returns NULL when invalid.
static cJSON *colors_canonical(const cJSON *colors)
{
    if (!cJSON_IsArray(colors)) return NULL;
    if (cJSON_GetArraySize(colors) > COLORS_MAX) return NULL;

    cJSON *out = cJSON_CreateArray();
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
            cJSON_Delete(out);
            return NULL;
        }
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", name->valuestring);
        cJSON_AddNumberToObject(entry, "r", r->valueint);
        cJSON_AddNumberToObject(entry, "g", g->valueint);
        cJSON_AddNumberToObject(entry, "b", b->valueint);
        cJSON_AddItemToArray(out, entry);
    }
    return out;
}

static esp_err_t colors_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *body = web_read_json(req, SETTINGS_BLOB_MAX);
    if (!body) return web_send_err(req, "invalid JSON body", 400);

    cJSON *canonical = colors_canonical(cJSON_GetObjectItem(body, "colors"));
    cJSON_Delete(body);
    if (!canonical) {
        return web_send_err(req,
                            "colors must be <=12 entries of "
                            "{name<=24ch,r,g,b 0..255}", 400);
    }

    cJSON *blob = cJSON_CreateObject();
    cJSON_AddNumberToObject(blob, "v", 1);
    cJSON_AddItemToObject(blob, "colors", canonical);
    char *json = cJSON_PrintUnformatted(blob);
    cJSON_Delete(blob);
    if (!json) return web_send_err(req, "out of memory", 500);

    esp_err_t err = settings_save_blob(COLORS_BLOB_KEY, json);
    cJSON_free(json);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving colors blob failed: %s", esp_err_to_name(err));
        return web_send_err(req, "saving colors failed", 500);
    }
    return web_send_ok(req);
}

// ---- /api/value ------------------------------------------------------------

static esp_err_t value_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *body = web_read_json(req, 512);
    if (!body) return web_send_err(req, "invalid JSON body", 400);

    const cJSON *val = cJSON_GetObjectItem(body, "value");
    char raw[DISPLAY_TEXT_MAX];
    if (cJSON_IsNumber(val)) {
        // Normalise JSON numbers so 4.2 and "4.2" behave the same; %.10g
        // keeps 10 significant digits without trailing-zero noise.
        snprintf(raw, sizeof(raw), "%.10g", val->valuedouble);
    } else if (cJSON_IsString(val)) {
        snprintf(raw, sizeof(raw), "%s", val->valuestring);
    } else {
        cJSON_Delete(body);
        return web_send_err(req, "body must be {\"value\":<string|number>}", 400);
    }
    cJSON_Delete(body);

    value_cfg_t vcfg;
    integrations_get_value_cfg(&vcfg);

    char text[DISPLAY_TEXT_MAX];
    valuefmt_format(&vcfg, raw, text, sizeof(text));

    // Always remembered (so switching to integration mode later shows it) and
    // always counted as fresh - but only RENDERED while the display source is
    // "integration". In static mode the panel keeps the user's text.
    integrations_set_last_value(text);
    integrations_touch_value();

    settings_t cfg;
    settings_load(&cfg);
    bool rendered = (cfg.source == DISPLAY_SRC_INTEGRATION);
    if (rendered) {
        // Show in the current colour, scrolling mode and speed; scale 0 =
        // keep the active scale. The mode is passed through rather than
        // forced off: a long formatted value is exactly what continuous
        // scrolling is for. Deliberately NOT persisted as the stored text -
        // that field is the static text now; after a reboot the value
        // config's default_text stands in until the next push.
        display_color_t color;
        uint8_t mode, speed;
        display_get_state(NULL, 0, &color, &mode, &speed, NULL);
        display_show_text(text, color, mode, speed, 0);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "displayed", text);
    cJSON_AddBoolToObject(resp, "rendered", rendered);
    return web_send_json(req, resp);
}

// ---- /api/value/config -----------------------------------------------------

static esp_err_t value_cfg_get(httpd_req_t *req)
{
    value_cfg_t v;
    integrations_get_value_cfg(&v);

    cJSON *r = cJSON_CreateObject();
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
    return web_send_json(req, r);
}

static esp_err_t value_cfg_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *body = web_read_json(req, 1024);
    if (!body) return web_send_err(req, "invalid JSON body", 400);

    // Overlay present fields onto the current config so clients can PATCH
    // a single field without knowing the whole schema.
    value_cfg_t v;
    integrations_get_value_cfg(&v);

    const cJSON *it;
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "prefix"))) {
        snprintf(v.prefix, sizeof(v.prefix), "%s", it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "suffix"))) {
        snprintf(v.suffix, sizeof(v.suffix), "%s", it->valuestring);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(body, "decimals"))) {
        int d = it->valueint;
        if (d < -1 || d > 9) {
            cJSON_Delete(body);
            return web_send_err(req, "decimals must be -1..9", 400);
        }
        v.decimals = (int8_t)d;
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "thousands"))) {
        v.thousands = it->valuestring[0];   // "" clears the separator
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(body, "stale_hours"))) {
        int h = it->valueint;
        if (h < 0 || h > 0xFFFF) {
            cJSON_Delete(body);
            return web_send_err(req, "stale_hours must be 0..65535", 400);
        }
        v.stale_hours = (uint16_t)h;
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "stale_action"))) {
        bool found = false;
        for (unsigned i = 0; i < sizeof(STALE_NAMES) / sizeof(STALE_NAMES[0]); i++) {
            if (strcmp(it->valuestring, STALE_NAMES[i]) == 0) {
                v.stale_action = (stale_action_t)i;
                found = true;
                break;
            }
        }
        if (!found) {
            cJSON_Delete(body);
            return web_send_err(req, "stale_action must be none|dim|marker|text", 400);
        }
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "stale_text"))) {
        snprintf(v.stale_text, sizeof(v.stale_text), "%s", it->valuestring);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(body, "default_text"))) {
        snprintf(v.default_text, sizeof(v.default_text), "%s", it->valuestring);
    }
    cJSON_Delete(body);

    esp_err_t err = integrations_set_value_cfg(&v);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving value config failed: %s", esp_err_to_name(err));
        return web_send_err(req, "saving value config failed", 500);
    }
    return web_send_ok(req);
}

// ---- registration ----------------------------------------------------------

void api_colors_register(httpd_handle_t srv)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/colors",       .method = HTTP_GET,  .handler = colors_get},
        {.uri = "/api/colors",       .method = HTTP_POST, .handler = colors_post},
        {.uri = "/api/value",        .method = HTTP_POST, .handler = value_post},
        {.uri = "/api/value/config", .method = HTTP_GET,  .handler = value_cfg_get},
        {.uri = "/api/value/config", .method = HTTP_POST, .handler = value_cfg_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
}
