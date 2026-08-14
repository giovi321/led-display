// /api/display/* handlers: text, brightness (+ cap), clear, test patterns,
// the legacy map-mode compatibility shim and the identify mode used by the
// arrangement editor. Endpoint ownership: api_handlers.h; response shapes:
// DESIGN.md section 5.

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "api_handlers.h"
#include "display.h"
#include "led_driver.h"
#include "mapping.h"
#include "scheduler.h"
#include "settings.h"
#include "web_common.h"

static const char *TAG = "api_display";

#define BODY_MAX 2048

static uint8_t clamp_u8(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

// Re-render the current text so a mapping change is visible immediately.
// scale=0 keeps the existing scale (display.h contract).
static void rerender_text(void)
{
    char            text[DISPLAY_TEXT_MAX];
    display_color_t col;
    bool            scroll;
    uint8_t         speed, scale;
    display_get_state(text, sizeof(text), &col, &scroll, &speed, &scale);
    if (text[0] != '\0') {
        display_show_text(text, col, scroll, speed, 0);
    }
}

static esp_err_t display_text_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const cJSON *t  = cJSON_GetObjectItem(json, "text");
    const cJSON *c  = cJSON_GetObjectItem(json, "color");
    const cJSON *s  = cJSON_GetObjectItem(json, "scroll");
    const cJSON *sp = cJSON_GetObjectItem(json, "speed");
    const cJSON *sc = cJSON_GetObjectItem(json, "scale");

    // Missing fields preserve the current state so a partial request from
    // curl or a third-party client can't silently reset everything.
    char            prev_text[DISPLAY_TEXT_MAX];
    display_color_t prev_col;
    bool            prev_scroll;
    uint8_t         prev_speed, prev_scale;
    display_get_state(prev_text, sizeof(prev_text),
                      &prev_col, &prev_scroll, &prev_speed, &prev_scale);

    // Clamp instead of wrapping: (uint8_t)300 would silently become 44 and
    // then get persisted, so out-of-range numbers pin to the valid bound.
    display_color_t col = prev_col;
    if (c && cJSON_IsObject(c)) {
        cJSON *cr = cJSON_GetObjectItem(c, "r");
        cJSON *cg = cJSON_GetObjectItem(c, "g");
        cJSON *cb = cJSON_GetObjectItem(c, "b");
        if (cJSON_IsNumber(cr)) col.r = clamp_u8(cr->valueint);
        if (cJSON_IsNumber(cg)) col.g = clamp_u8(cg->valueint);
        if (cJSON_IsNumber(cb)) col.b = clamp_u8(cb->valueint);
    }
    bool    scroll = cJSON_IsBool(s)    ? cJSON_IsTrue(s)          : prev_scroll;
    uint8_t speed  = cJSON_IsNumber(sp) ? clamp_u8(sp->valueint)   : prev_speed;
    uint8_t scale  = 0;   // 0 = keep current
    if (cJSON_IsNumber(sc)) {
        int v = sc->valueint;
        if (v < 0) v = 0;
        if (v > FONT_SCALE_MAX) v = FONT_SCALE_MAX;
        scale = (uint8_t)v;
    }
    const char *text = cJSON_IsString(t) ? t->valuestring : prev_text;

    // The display truncates its RAM copy to DISPLAY_TEXT_MAX, but NVS would
    // happily store a longer string that settings_load() then cannot read
    // back, silently losing the text on the next boot. Truncate both paths.
    char text_buf[DISPLAY_TEXT_MAX];
    snprintf(text_buf, sizeof(text_buf), "%s", text);

    display_show_text(text_buf, col, scroll, speed, scale);
    settings_save_text(text_buf, &col, scroll, speed, scale);
    cJSON_Delete(json);
    return web_send_ok(req);
}

static esp_err_t display_brightness_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const cJSON *b = cJSON_GetObjectItem(json, "brightness");
    if (cJSON_IsNumber(b)) {
        int v = b->valueint;
        if (v < 0)   v = 0;
        if (v > 255) v = 255;
        // The cap is the power-budget guard from the advanced UI; requests
        // above it are silently clamped rather than rejected so old clients
        // keep working after a cap is configured.
        uint8_t cap = led_driver_get_brightness_cap();
        if (v > cap) v = cap;
        // During a scheduled-off window only persist the value: applying it
        // live would relight the panel until the next on/off transition
        // (the scheduler models "off" as brightness 0 and early-returns).
        // The scheduler picks the new value up from NVS when it turns the
        // display back on. Same guard as the config-import path.
        settings_save_brightness((uint8_t)v);
        if (!scheduler_display_is_off()) {
            led_driver_set_brightness((uint8_t)v);
            led_driver_lock();
            led_driver_refresh();
            led_driver_unlock();
        }
    }
    cJSON_Delete(json);
    return web_send_ok(req);
}

static esp_err_t display_brightness_cap_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const cJSON *c = cJSON_GetObjectItem(json, "cap");
    if (!cJSON_IsNumber(c)) {
        cJSON_Delete(json);
        return web_send_err(req, "cap required", 400);
    }
    int v = c->valueint;
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    cJSON_Delete(json);

    led_driver_set_brightness_cap((uint8_t)v);
    settings_save_brightness_cap((uint8_t)v);
    // Refresh so a cap below the current brightness takes effect visibly.
    led_driver_lock();
    led_driver_refresh();
    led_driver_unlock();
    return web_send_ok(req);
}

static esp_err_t display_clear_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    display_clear();
    settings_clear_text();
    return web_send_ok(req);
}

static esp_err_t display_test_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const cJSON *p = cJSON_GetObjectItem(json, "pattern");
    const char *name = cJSON_IsString(p) ? p->valuestring : "step_raw";

    display_test_t which = DISPLAY_TEST_STEP_RAW;
    if      (!strcmp(name, "step_raw")) which = DISPLAY_TEST_STEP_RAW;
    else if (!strcmp(name, "step_xy"))  which = DISPLAY_TEST_STEP_XY;
    else if (!strcmp(name, "corners"))  which = DISPLAY_TEST_CORNERS;
    else if (!strcmp(name, "border"))   which = DISPLAY_TEST_BORDER;
    else if (!strcmp(name, "fill"))     which = DISPLAY_TEST_FILL;
    cJSON_Delete(json);

    display_run_test(which);
    return web_send_ok(req);
}

// Compatibility shim for the v1 map-mode API: the mode names map onto the
// legacy led_map_mode_t values 0..6, which mapping_from_legacy() turns into
// equivalent runtime layouts. New clients should use POST /api/layout.
static esp_err_t display_map_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const cJSON *m = cJSON_GetObjectItem(json, "mode");
    const char *name = cJSON_IsString(m) ? m->valuestring : "";

    int legacy = -1;
    if      (!strcmp(name, "row"))               legacy = 0;
    else if (!strcmp(name, "serp"))              legacy = 1;
    else if (!strcmp(name, "row_bl"))            legacy = 2;
    else if (!strcmp(name, "serp_bl"))           legacy = 3;
    else if (!strcmp(name, "col"))               legacy = 4;
    else if (!strcmp(name, "col_serp"))          legacy = 5;
    else if (!strcmp(name, "grid_2x2_col_serp")) legacy = 6;
    cJSON_Delete(json);

    mapping_config_t cfg;
    if (legacy < 0 || mapping_from_legacy(legacy, &cfg) != MAPPING_OK) {
        return web_send_err(req, "unknown mode", 400);
    }

    esp_err_t err = led_driver_reconfigure(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "map shim reconfigure failed: %s", esp_err_to_name(err));
        return web_send_err(req, "reconfigure failed", 500);
    }
    settings_save_layout(&cfg);
    // Re-render the current text under the new mapping so the user sees it
    // immediately (same behaviour as v1).
    rerender_text();
    return web_send_ok(req);
}

static esp_err_t display_identify_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const cJSON *on = cJSON_GetObjectItem(json, "on");
    if (!cJSON_IsBool(on)) {
        cJSON_Delete(json);
        return web_send_err(req, "on required", 400);
    }
    bool enable = cJSON_IsTrue(on);
    cJSON_Delete(json);

    display_identify(enable);
    return web_send_ok(req);
}

void api_display_register(httpd_handle_t srv)
{
    static const httpd_uri_t uris[] = {
        {.uri = "/api/display/text",           .method = HTTP_POST, .handler = display_text_post},
        {.uri = "/api/display/brightness",     .method = HTTP_POST, .handler = display_brightness_post},
        {.uri = "/api/display/brightness_cap", .method = HTTP_POST, .handler = display_brightness_cap_post},
        {.uri = "/api/display/clear",          .method = HTTP_POST, .handler = display_clear_post},
        {.uri = "/api/display/test",           .method = HTTP_POST, .handler = display_test_post},
        {.uri = "/api/display/map",            .method = HTTP_POST, .handler = display_map_post},
        {.uri = "/api/display/identify",       .method = HTTP_POST, .handler = display_identify_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(srv, &uris[i]);
    }
}
