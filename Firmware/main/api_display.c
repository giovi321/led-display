// /api/display/* handlers: text, brightness (+ cap), clear, test patterns,
// the legacy map-mode compatibility shim and the identify mode used by the
// arrangement editor. Endpoint ownership: api_handlers.h; response shapes:
// DESIGN.md section 5.

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "api_handlers.h"
#include "display.h"
#include "integrations.h"
#include "led_driver.h"
#include "light_sensor.h"
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
    uint8_t         mode, speed, scale;
    display_get_state(text, sizeof(text), &col, &mode, &speed, &scale);
    if (text[0] != '\0') {
        display_show_text(text, col, mode, speed, 0);
    }
}

// Scroll-mode names on the wire. Index == display_scroll_t value.
static const char *SCROLL_NAMES[] = {"off", "restart", "continuous"};

void api_display_apply_source(void)
{
    settings_t cfg;
    settings_load(&cfg);

    char text[DISPLAY_TEXT_MAX];
    if (cfg.source == DISPLAY_SRC_INTEGRATION) {
        // Latest pushed value; the configured placeholder until one arrives.
        if (!integrations_get_last_value(text, sizeof(text))) {
            value_cfg_t vc;
            integrations_get_value_cfg(&vc);
            snprintf(text, sizeof(text), "%s", vc.fb.text);
        }
    } else {
        snprintf(text, sizeof(text), "%s", cfg.text);
    }

    if (text[0] != '\0') {
        display_show_text(text, cfg.color, cfg.scroll_mode, cfg.speed,
                          cfg.scale);
    } else {
        // Nothing to show; display_clear also ends identify/test modes.
        display_clear();
    }
}

static int scroll_mode_by_name(const char *name)
{
    for (unsigned i = 0; i < sizeof(SCROLL_NAMES) / sizeof(SCROLL_NAMES[0]); i++) {
        if (strcmp(name, SCROLL_NAMES[i]) == 0) return (int)i;
    }
    return -1;
}

static esp_err_t display_text_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const cJSON *t  = cJSON_GetObjectItem(json, "text");
    const cJSON *c  = cJSON_GetObjectItem(json, "color");
    const cJSON *s  = cJSON_GetObjectItem(json, "scroll");
    const cJSON *sm = cJSON_GetObjectItem(json, "scroll_mode");
    const cJSON *sp = cJSON_GetObjectItem(json, "speed");
    const cJSON *sc = cJSON_GetObjectItem(json, "scale");
    const cJSON *fo = cJSON_GetObjectItem(json, "font");
    const cJSON *sg = cJSON_GetObjectItem(json, "spacing");

    // Missing fields preserve the current state so a partial request from
    // curl or a third-party client can't silently reset everything.
    char            prev_text[DISPLAY_TEXT_MAX];
    display_color_t prev_col;
    uint8_t         prev_mode, prev_speed, prev_scale;
    display_get_state(prev_text, sizeof(prev_text),
                      &prev_col, &prev_mode, &prev_speed, &prev_scale);

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
    // Mode resolution order: the explicit scroll_mode string wins; the
    // legacy scroll bool only switches off/on so a v1 client that always
    // sends scroll:true can't silently downgrade a continuous marquee.
    uint8_t mode = prev_mode;
    if (cJSON_IsBool(s)) {
        if (!cJSON_IsTrue(s))      mode = DISPLAY_SCROLL_OFF;
        else if (prev_mode == DISPLAY_SCROLL_OFF) mode = DISPLAY_SCROLL_RESTART;
    }
    if (cJSON_IsString(sm)) {
        int m = scroll_mode_by_name(sm->valuestring);
        if (m < 0) {
            cJSON_Delete(json);
            return web_send_err(req, "scroll_mode must be off|restart|continuous", 400);
        }
        mode = (uint8_t)m;
    }
    uint8_t speed  = cJSON_IsNumber(sp) ? clamp_u8(sp->valueint)   : prev_speed;
    uint8_t scale  = 0;   // 0 = keep current
    if (cJSON_IsNumber(sc)) {
        int v = sc->valueint;
        if (v < 0) v = 0;
        if (v > FONT_SCALE_MAX) v = FONT_SCALE_MAX;
        scale = (uint8_t)v;
    }

    // Font + spacing ride on the same endpoint; absent fields keep the
    // current values, like everything else here.
    uint8_t prev_font;
    bool    prev_prop;
    display_get_font(&prev_font, &prev_prop);
    int  font_id = prev_font;
    bool prop    = prev_prop;
    if (cJSON_IsString(fo)) {
        font_id = font_id_by_name(fo->valuestring);
        if (font_id < 0) {
            cJSON_Delete(json);
            return web_send_err(req, "font must be 5x7|3x5|7x12", 400);
        }
    }
    if (cJSON_IsString(sg)) {
        if      (!strcmp(sg->valuestring, "proportional")) prop = true;
        else if (!strcmp(sg->valuestring, "monospace"))    prop = false;
        else {
            cJSON_Delete(json);
            return web_send_err(req, "spacing must be proportional|monospace", 400);
        }
    }
    const bool has_text = cJSON_IsString(t);
    const char *text = has_text ? t->valuestring : prev_text;

    // The display truncates its RAM copy to DISPLAY_TEXT_MAX, but NVS would
    // happily store a longer string that settings_load() then cannot read
    // back, silently losing the text on the next boot. Truncate both paths.
    char text_buf[DISPLAY_TEXT_MAX];
    snprintf(text_buf, sizeof(text_buf), "%s", text);

    if ((uint8_t)font_id != prev_font || prop != prev_prop) {
        display_set_font((uint8_t)font_id, prop);
        settings_save_font((uint8_t)font_id, prop);
    }
    display_show_text(text_buf, col, mode, speed, scale);
    // A formatting-only POST must not rewrite the stored text key: in
    // integration mode the on-screen text is the pushed value, not the
    // user's static text, and persisting it here would clobber the latter.
    settings_save_text(has_text ? text_buf : NULL, &col, mode, speed, scale);
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
        // Persist, then let the arbiter decide what actually reaches the panel:
        // inside a scheduled-off window the stored value only takes effect at
        // the next on transition, and while the light sensor is driving the
        // brightness it stays stored but unapplied.
        settings_save_brightness((uint8_t)v);
        scheduler_reapply_brightness();
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
    // Re-derive through the arbiter rather than just refreshing: the level is
    // clamped to the cap before the staleness dim, so raising the cap has to
    // recompute and not re-push the old clamped value.
    scheduler_reapply_brightness();
    return web_send_ok(req);
}

// ---- /api/display/power ----------------------------------------------------
//
// The calculated power budget: the model's constants, the budget to hold to and
// whether the driver enforces it. Lives next to brightness_cap because it is
// the same concern done properly - the cap scales RGB blind, this one prices
// the frame first. The live estimate is reported in GET /api/status, where the
// UI is already polling.

static esp_err_t display_power_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    // Absent fields keep their stored value, like the other POSTs here.
    power_cfg_t cfg;
    led_driver_get_power_cfg(&cfg);

    const cJSON *it;
    if (cJSON_IsBool(it = cJSON_GetObjectItem(json, "limit_enabled"))) {
        cfg.limit_enabled = cJSON_IsTrue(it);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(json, "budget_ma"))) {
        if (it->valueint < 0 || it->valueint > POWER_BUDGET_MA_MAX) {
            cJSON_Delete(json);
            return web_send_err(req, "budget_ma must be 0-25000 (0 = no budget)", 400);
        }
        cfg.budget_ma = (uint16_t)it->valueint;
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(json, "ua_per_channel"))) {
        if (it->valueint < POWER_UA_PER_CHANNEL_MIN ||
            it->valueint > POWER_UA_PER_CHANNEL_MAX) {
            cJSON_Delete(json);
            return web_send_err(req, "ua_per_channel must be 1-40000", 400);
        }
        cfg.ua_per_channel = (uint16_t)it->valueint;
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(json, "ua_quiescent"))) {
        if (it->valueint < 0 || it->valueint > POWER_UA_QUIESCENT_MAX) {
            cJSON_Delete(json);
            return web_send_err(req, "ua_quiescent must be 0-5000", 400);
        }
        cfg.ua_quiescent = (uint16_t)it->valueint;
    }
    cJSON_Delete(json);

    // Enforcing a budget of 0 would mean "limit to nothing", which reads as a
    // dark panel and is never what the caller wanted.
    if (cfg.limit_enabled && cfg.budget_ma == 0) {
        return web_send_err(req, "budget_ma must be set before enabling the limiter", 400);
    }
    if (!power_model_valid(&cfg)) {
        return web_send_err(req, "power config out of range", 400);
    }

    led_driver_set_power_cfg(&cfg);
    esp_err_t err = settings_save_power(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving power budget failed: %s", esp_err_to_name(err));
        return web_send_err(req, "saving power budget failed", 500);
    }
    // The limiter acts per frame, so a new budget takes hold on the next
    // refresh with no repaint needed. Re-derive the brightness anyway: the
    // arbiter owns the level, and a released limiter should not wait for the
    // next content change to give it back.
    scheduler_reapply_brightness();
    return web_send_ok(req);
}

// ---- /api/display/light_sensor ---------------------------------------------
//
// Lives under the display prefix next to brightness_cap: it is another input to
// the same brightness, configured from the Hardware tab. The GET also carries
// the live reading, which is what makes the two calibration points settable
// without a multimeter.

static void light_sensor_json(cJSON *r, const light_sensor_cfg_t *c)
{
    cJSON_AddBoolToObject(r, "enabled", c->enabled);
    cJSON_AddNumberToObject(r, "sda", c->sda);
    cJSON_AddNumberToObject(r, "scl", c->scl);
    cJSON_AddNumberToObject(r, "dark_raw", c->curve.dark_raw);
    cJSON_AddNumberToObject(r, "bright_raw", c->curve.bright_raw);
    cJSON_AddNumberToObject(r, "min_brightness", c->curve.min_bri);
    cJSON_AddNumberToObject(r, "max_brightness", c->curve.max_bri);
    // Tenths on the wire as well: no float rounding surprises in a config file.
    cJSON_AddNumberToObject(r, "gamma_x10", c->curve.gamma_x10);
}

static esp_err_t light_sensor_get_handler(httpd_req_t *req)
{
    light_sensor_cfg_t cfg;
    light_sensor_status_t st;
    light_sensor_get(&cfg);
    light_sensor_get_status(&st);

    cJSON *r = cJSON_CreateObject();
    light_sensor_json(r, &cfg);
    cJSON_AddBoolToObject(r, "sampling", st.sampling);
    cJSON_AddNumberToObject(r, "raw", st.raw);       // -1 = nothing read yet
    cJSON_AddNumberToObject(r, "level", st.level);   // -1 = not driving
    cJSON_AddNumberToObject(r, "raw_max", LIGHT_RAW_MAX);
    // Every pin the BH1750's SDA or SCL may use, so the UI never has to carry
    // its own copy of the strapping and flash-pin rules.
    cJSON *i2c = cJSON_AddArrayToObject(r, "valid_i2c_gpios");
    for (int g = 0; g <= 48; g++) {
        if (light_curve_valid_i2c_gpio(g)) {
            cJSON_AddItemToArray(i2c, cJSON_CreateNumber(g));
        }
    }
    return web_send_json(req, r);
}

static esp_err_t light_sensor_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    // Absent fields keep their stored value, like the other POSTs here.
    light_sensor_cfg_t cfg;
    light_sensor_get(&cfg);

    const cJSON *it;
    if (cJSON_IsBool(it = cJSON_GetObjectItem(json, "enabled"))) {
        cfg.enabled = cJSON_IsTrue(it);
    }
    struct { const char *key; uint8_t *dst; } i2c_pins[] = {
        {"sda", &cfg.sda},
        {"scl", &cfg.scl},
    };
    for (unsigned i = 0; i < sizeof(i2c_pins) / sizeof(i2c_pins[0]); i++) {
        if (!cJSON_IsNumber(it = cJSON_GetObjectItem(json, i2c_pins[i].key))) continue;
        if (!light_curve_valid_i2c_gpio(it->valueint)) {
            cJSON_Delete(json);
            return web_send_err(req,
                                "sda/scl must be a free GPIO: not 0/3/45/46 "
                                "(strapping), not 22-25 (absent), not 26-37 "
                                "(flash and PSRAM)", 400);
        }
        *i2c_pins[i].dst = (uint8_t)it->valueint;
    }
    struct { const char *key; uint16_t *dst; } raws[] = {
        {"dark_raw",   &cfg.curve.dark_raw},
        {"bright_raw", &cfg.curve.bright_raw},
    };
    for (unsigned i = 0; i < sizeof(raws) / sizeof(raws[0]); i++) {
        if (!cJSON_IsNumber(it = cJSON_GetObjectItem(json, raws[i].key))) continue;
        if (it->valueint < 0 || it->valueint > LIGHT_RAW_MAX) {
            cJSON_Delete(json);
            return web_send_err(req, "dark_raw/bright_raw must be 0-4095", 400);
        }
        *raws[i].dst = (uint16_t)it->valueint;
    }
    struct { const char *key; uint8_t *dst; } bris[] = {
        {"min_brightness", &cfg.curve.min_bri},
        {"max_brightness", &cfg.curve.max_bri},
    };
    for (unsigned i = 0; i < sizeof(bris) / sizeof(bris[0]); i++) {
        if (!cJSON_IsNumber(it = cJSON_GetObjectItem(json, bris[i].key))) continue;
        if (it->valueint < 0 || it->valueint > 255) {
            cJSON_Delete(json);
            return web_send_err(req, "min_brightness/max_brightness must be 0-255", 400);
        }
        *bris[i].dst = (uint8_t)it->valueint;
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(json, "gamma_x10"))) {
        if (it->valueint < LIGHT_GAMMA_MIN || it->valueint > LIGHT_GAMMA_MAX) {
            cJSON_Delete(json);
            return web_send_err(req, "gamma_x10 must be 5-40 (0.5-4.0)", 400);
        }
        cfg.curve.gamma_x10 = (uint8_t)it->valueint;
    }
    cJSON_Delete(json);

    esp_err_t err = light_sensor_set(&cfg);
    if (err == ESP_ERR_INVALID_ARG) {
        return web_send_err(req,
                            "dark_raw and bright_raw must differ, and SDA and "
                            "SCL must be different pins", 400);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving light sensor config failed: %s", esp_err_to_name(err));
        return web_send_err(req, "saving light sensor config failed", 500);
    }
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

    // "off" ends whatever test or identify mode is active and puts the
    // configured content (static text or integration value) back.
    if (!strcmp(name, "off")) {
        cJSON_Delete(json);
        api_display_apply_source();
        return web_send_ok(req);
    }

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

// Switch the display between the stored static text and the integration
// value (with the fallback text before the first push). The new
// content renders immediately.
static esp_err_t display_source_post(httpd_req_t *req)
{
    if (web_require_auth(req) != ESP_OK) return ESP_OK;

    cJSON *json = web_read_json(req, BODY_MAX);
    if (!json) return web_send_err(req, "bad json", 400);

    const cJSON *m = cJSON_GetObjectItem(json, "mode");
    const char *name = cJSON_IsString(m) ? m->valuestring : "";

    display_source_t src;
    if      (!strcmp(name, "static"))      src = DISPLAY_SRC_STATIC;
    else if (!strcmp(name, "integration")) src = DISPLAY_SRC_INTEGRATION;
    else {
        cJSON_Delete(json);
        return web_send_err(req, "mode must be static|integration", 400);
    }
    cJSON_Delete(json);

    esp_err_t err = settings_save_source(src);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving display source failed: %s", esp_err_to_name(err));
        return web_send_err(req, "saving display source failed", 500);
    }
    api_display_apply_source();
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
        {.uri = "/api/display/power",          .method = HTTP_POST, .handler = display_power_post},
        {.uri = "/api/display/light_sensor",   .method = HTTP_GET,  .handler = light_sensor_get_handler},
        {.uri = "/api/display/light_sensor",   .method = HTTP_POST, .handler = light_sensor_post},
        {.uri = "/api/display/clear",          .method = HTTP_POST, .handler = display_clear_post},
        {.uri = "/api/display/test",           .method = HTTP_POST, .handler = display_test_post},
        {.uri = "/api/display/map",            .method = HTTP_POST, .handler = display_map_post},
        {.uri = "/api/display/identify",       .method = HTTP_POST, .handler = display_identify_post},
        {.uri = "/api/display/source",         .method = HTTP_POST, .handler = display_source_post},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        web_register_uri(srv, &uris[i]);
    }
}
