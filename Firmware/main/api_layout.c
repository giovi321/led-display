// /api/layout GET/POST: full panel-arrangement config for the graphical
// editor. JSON shape is fixed by DESIGN.md section 5; the web UI is written
// against these exact field names. Endpoint ownership: api_handlers.h.

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "api_handlers.h"
#include "display.h"
#include "led_driver.h"
#include "mapping.h"
#include "settings.h"
#include "web_common.h"

static const char *TAG = "api_layout";

// A valid layout is <=32 panels (~40 bytes of JSON per cell) plus panel and
// grid objects; 4 KB leaves headroom without letting a client feed us more.
#define LAYOUT_BODY_MAX 4096

// ---- serialisation --------------------------------------------------------

static const char *origin_str(map_origin_t o)
{
    switch (o) {
        case MAP_ORIGIN_TR: return "TR";
        case MAP_ORIGIN_BL: return "BL";
        case MAP_ORIGIN_BR: return "BR";
        default:            return "TL";
    }
}

static bool origin_parse(const char *s, map_origin_t *out)
{
    if      (!strcmp(s, "TL")) *out = MAP_ORIGIN_TL;
    else if (!strcmp(s, "TR")) *out = MAP_ORIGIN_TR;
    else if (!strcmp(s, "BL")) *out = MAP_ORIGIN_BL;
    else if (!strcmp(s, "BR")) *out = MAP_ORIGIN_BR;
    else return false;
    return true;
}

// Parse a JSON number into a uint8_t, rejecting out-of-range values instead
// of letting the cast wrap (a wrapped value could sneak past
// mapping_validate() with a different meaning than the client sent).
static bool json_u8(const cJSON *item, uint8_t *out)
{
    if (!cJSON_IsNumber(item)) return false;
    int v = item->valueint;
    if (v < 0 || v > 255) return false;
    *out = (uint8_t)v;
    return true;
}

static esp_err_t layout_get(httpd_req_t *req)
{
    mapping_config_t cfg;
    led_driver_get_config(&cfg);

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "v", 1);

    cJSON *p = cJSON_AddObjectToObject(r, "panel");
    cJSON_AddNumberToObject(p, "w", cfg.panel.w);
    cJSON_AddNumberToObject(p, "h", cfg.panel.h);
    cJSON_AddStringToObject(p, "origin", origin_str(cfg.panel.origin));
    cJSON_AddStringToObject(p, "axis", cfg.panel.axis == MAP_AXIS_COL ? "col" : "row");
    cJSON_AddBoolToObject(p,   "serpentine", cfg.panel.serpentine);

    cJSON *g = cJSON_AddObjectToObject(r, "grid");
    cJSON_AddNumberToObject(g, "rows", cfg.rows);
    cJSON_AddNumberToObject(g, "cols", cfg.cols);

    cJSON *cells = cJSON_AddArrayToObject(r, "cells");
    for (int row = 0; row < cfg.rows; row++) {
        for (int col = 0; col < cfg.cols; col++) {
            cJSON *cell = cJSON_CreateObject();
            cJSON_AddNumberToObject(cell, "row", row);
            cJSON_AddNumberToObject(cell, "col", col);
            cJSON_AddNumberToObject(cell, "chain", cfg.cells[row][col].chain);
            cJSON_AddNumberToObject(cell, "rot", cfg.cells[row][col].rot);
            cJSON_AddItemToArray(cells, cell);
        }
    }

    cJSON_AddNumberToObject(r, "width",  mapping_width(&cfg));
    cJSON_AddNumberToObject(r, "height", mapping_height(&cfg));

    // Limits let the editor bound its steppers without hardcoding them.
    cJSON *lim = cJSON_AddObjectToObject(r, "limits");
    cJSON_AddNumberToObject(lim, "max_rows", MAP_MAX_ROWS);
    cJSON_AddNumberToObject(lim, "max_cols", MAP_MAX_COLS);
    cJSON_AddNumberToObject(lim, "max_panels", MAP_MAX_PANELS);
    cJSON_AddNumberToObject(lim, "max_pixels", MAP_MAX_PIXELS);

    return web_send_json(req, r);
}

// ---- POST ------------------------------------------------------------------

// Parse the GET shape back into a mapping_config_t. width/height/limits (and
// "v") are derived/informational and deliberately ignored on input. Returns
// NULL on success or a 400 error string.
static const char *layout_parse(const cJSON *json, mapping_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    const cJSON *p     = cJSON_GetObjectItem(json, "panel");
    const cJSON *g     = cJSON_GetObjectItem(json, "grid");
    const cJSON *cells = cJSON_GetObjectItem(json, "cells");
    if (!cJSON_IsObject(p) || !cJSON_IsObject(g) || !cJSON_IsArray(cells)) {
        return "panel, grid and cells required";
    }

    if (!json_u8(cJSON_GetObjectItem(p, "w"), &cfg->panel.w) ||
        !json_u8(cJSON_GetObjectItem(p, "h"), &cfg->panel.h)) {
        return "panel w/h must be numbers 1..64";
    }
    const cJSON *origin = cJSON_GetObjectItem(p, "origin");
    if (!cJSON_IsString(origin) ||
        !origin_parse(origin->valuestring, &cfg->panel.origin)) {
        return "panel origin must be TL, TR, BL or BR";
    }
    const cJSON *axis = cJSON_GetObjectItem(p, "axis");
    if (cJSON_IsString(axis) && !strcmp(axis->valuestring, "row")) {
        cfg->panel.axis = MAP_AXIS_ROW;
    } else if (cJSON_IsString(axis) && !strcmp(axis->valuestring, "col")) {
        cfg->panel.axis = MAP_AXIS_COL;
    } else {
        return "panel axis must be row or col";
    }
    const cJSON *serp = cJSON_GetObjectItem(p, "serpentine");
    if (!cJSON_IsBool(serp)) {
        return "panel serpentine must be a bool";
    }
    cfg->panel.serpentine = cJSON_IsTrue(serp);

    if (!json_u8(cJSON_GetObjectItem(g, "rows"), &cfg->rows) ||
        !json_u8(cJSON_GetObjectItem(g, "cols"), &cfg->cols)) {
        return "grid rows/cols must be numbers";
    }

    const cJSON *cell;
    cJSON_ArrayForEach(cell, cells) {
        uint8_t row, col, chain;
        if (!cJSON_IsObject(cell) ||
            !json_u8(cJSON_GetObjectItem(cell, "row"), &row) ||
            !json_u8(cJSON_GetObjectItem(cell, "col"), &col) ||
            !json_u8(cJSON_GetObjectItem(cell, "chain"), &chain)) {
            return "each cell needs numeric row, col and chain";
        }
        // Bound against the array dimensions BEFORE indexing: cfg->rows/cols
        // are validated later, so a hostile row/col must not write out of
        // bounds here.
        if (row >= cfg->rows || row >= MAP_MAX_ROWS ||
            col >= cfg->cols || col >= MAP_MAX_COLS) {
            return "cell row/col outside the grid";
        }
        const cJSON *rot = cJSON_GetObjectItem(cell, "rot");
        if (!cJSON_IsNumber(rot) || rot->valueint < 0 || rot->valueint > 270) {
            return "cell rot must be 0, 90, 180 or 270";
        }
        cfg->cells[row][col].chain = chain;
        cfg->cells[row][col].rot   = (uint16_t)rot->valueint;
    }
    return NULL;
}

static esp_err_t layout_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, LAYOUT_BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    mapping_config_t cfg;
    const char *parse_err = layout_parse(json, &cfg);
    cJSON_Delete(json);
    if (parse_err) {
        return web_send_err(req, parse_err, 400);
    }

    int rc = mapping_validate(&cfg);
    if (rc != MAPPING_OK) {
        const char *msg =
            rc == MAPPING_ERR_CHAIN ? "chain indices must be a permutation of 0..n-1" :
            rc == MAPPING_ERR_ROT   ? "rot must be 0/90/180/270; 90/270 only on square panels" :
                                      "rows/cols/panel size out of range or over the pixel cap";
        return web_send_err(req, msg, 400);
    }

    esp_err_t err = led_driver_reconfigure(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reconfigure failed: %s", esp_err_to_name(err));
        return web_send_err(req, "reconfigure failed", 500);
    }
    err = settings_save_layout(&cfg);
    if (err != ESP_OK) {
        // Applied but not persisted: keep going, the layout works until
        // reboot and the user can re-apply; losing the request would be worse.
        ESP_LOGW(TAG, "layout applied but not persisted: %s", esp_err_to_name(err));
    }

    // Visual confirmation of the new edges, without going through
    // display_run_test: the test task clears the stored text state to pause
    // the scroll task, which would wipe the content we are about to restore.
    // draw_border touches only the frame buffer, so border for a second and
    // then re-render the stored text under the new mapping.
    display_draw_border((display_color_t){80, 32, 0});
    vTaskDelay(pdMS_TO_TICKS(1000));

    char            text[DISPLAY_TEXT_MAX];
    display_color_t col;
    uint8_t         mode, speed, scale;
    display_get_state(text, sizeof(text), &col, &mode, &speed, &scale);
    if (text[0] != '\0') {
        display_show_text(text, col, mode, speed, 0);   // 0 = keep scale
    } else {
        display_clear();
    }

    return web_send_ok(req);
}

void api_layout_register(httpd_handle_t srv)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/layout", .method = HTTP_GET,  .handler = layout_get},
        {.uri = "/api/layout", .method = HTTP_POST, .handler = layout_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
}
