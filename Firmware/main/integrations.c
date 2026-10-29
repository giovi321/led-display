// Integration settings (see integrations.h): RAM cache in front of two JSON
// blobs in the "cfg" NVS namespace. Reads must stay cheap and flash-free
// because web_auth_ok() calls integrations_get() on every mutating request
// and the scheduler polls the value config every 30 s.
//
// The device name is a special case: from 2.2 it is ONE value shared with the
// network hostname and the setup-AP SSID, stored as the scalar NVS key "dname"
// (settings.c). This module still owns the RAM copy, because the webhook worker
// reads it on every delivery, and it stays mirrored into the "integr" blob so an
// OTA rollback to an older image still finds a webhook name.
//
// Blob shapes (versioned for forward migration, DESIGN.md section 4):
//   "integr"   {"v":1,"api_key":"..","webhook_url":"..",
//               "webhook_events":<mask>,"device_name":"..",
//               "ui_auth":false,"ui_user":"..","ui_pass":"..",
//               "webhook_hdr_name":"..",
//               "webhook_hdr_value":"..","heartbeat_min":0}
//   "valuecfg" {"v":2,"prefix":"..","suffix":"..","decimals":-1,
//               "thousands":"","multiplier":1,"scale":"none",
//               "scale_letters":"k M B T","dec_sep":".","plus_sign":false,
//               "fb_text":"..","fb_mode":"text","fb_after_min":0,
//               "fb_poll_failures":3,"fb_on_mqtt_loss":false}
//   A v1 valuecfg blob (<= 2.11.0: stale_hours, stale_action, stale_text,
//   default_text) is migrated through fb_migrate_legacy on load and re-saved
//   as v2.

#include "integrations.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "devname.h"
#include "settings.h"
#include "wifi_manager.h"

static const char *TAG = "integr";

#define BLOB_INTEGR   "integr"
#define BLOB_VALUECFG "valuecfg"
#define VALUECFG_VERSION 2
#define SCALE_LETTERS_DEFAULT "k M B T"

#define DEVICE_NAME_DEFAULT DEVNAME_DEFAULT_HOST

// Static initialisers double as the pre-init defaults: web_auth_ok() may run
// before integrations_init() finishes and must see "no key = open".
static SemaphoreHandle_t  s_lock;
static integrations_cfg_t s_cfg       = {.device_name = DEVICE_NAME_DEFAULT};
static value_cfg_t        s_value_cfg = {.decimals = -1, .multiplier = 1.0,
                                         .dec_sep = '.',
                                         .fb = {.poll_failures = 3}};
static int64_t            s_last_value_us = -1;   // -1 = no value yet
static char               s_last_value_text[VALUE_TEXT_MAX * 2 + 1];
static bool               s_last_value_set;

// value_scale_t <-> blob/API string. Index == value_scale_t.
static const char *SCALE_NAMES[] = {"none", "auto", "k", "M", "B"};
#define SCALE_COUNT (sizeof(SCALE_NAMES) / sizeof(SCALE_NAMES[0]))

// Pre-2.12 stale_action strings. Index == the stale_action code that
// fb_migrate_legacy takes (0 none, 1 dim, 2 marker, 3 text).
static const char *LEGACY_ACT_NAMES[] = {"none", "dim", "marker", "text"};
#define LEGACY_ACT_COUNT (sizeof(LEGACY_ACT_NAMES) / sizeof(LEGACY_ACT_NAMES[0]))

// Tolerate pre-init callers: without the mutex we just read/write the
// statics directly, which is safe enough for boot-time single-task access.
static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

static void cfg_defaults(integrations_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->device_name, sizeof(c->device_name), DEVICE_NAME_DEFAULT);
}

static void value_cfg_defaults(value_cfg_t *v)
{
    memset(v, 0, sizeof(*v));
    v->decimals   = -1;                // leave values as sent
    v->multiplier = 1.0;
    v->scale      = VALUE_SCALE_NONE;
    v->dec_sep    = '.';
    snprintf(v->scale_letters, sizeof(v->scale_letters), SCALE_LETTERS_DEFAULT);
    fb_cfg_defaults(&v->fb);
}

// ---- blob (de)serialisation ------------------------------------------------

static void json_get_str(const cJSON *root, const char *key,
                         char *dst, size_t dstsz)
{
    const cJSON *it = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(it)) snprintf(dst, dstsz, "%s", it->valuestring);
}

// ---- value config <-> JSON (blob, API, backup) -----------------------------

// Strict mode returns the message; lenient mode skips the field. Used inside
// integrations_value_cfg_from_json only.
#define VC_FAIL(msg)                       \
    do {                                   \
        if (strict) {                      \
            if (err) *err = (msg);         \
            return false;                  \
        }                                  \
    } while (0)

// A number in [lo, hi]. Lenient callers get out-of-range numbers clamped;
// returns false (field to be rejected or skipped) only for a non-number, or
// for an out-of-range number in strict mode.
static bool vc_num(const cJSON *it, double lo, double hi, bool strict,
                   double *out)
{
    if (!cJSON_IsNumber(it)) return false;
    double d = it->valuedouble;
    if (isnan(d)) return false;
    if (d < lo || d > hi) {
        if (strict) return false;
        d = d < lo ? lo : hi;
    }
    *out = d;
    return true;
}

static int legacy_act_of(const fallback_cfg_t *fb)
{
    if (fb->after_min == 0) return 0;
    switch (fb->mode) {
        case FB_MODE_DIM:    return 1;
        case FB_MODE_MARKER: return 2;
        default:             return 3;
    }
}

bool integrations_value_cfg_from_json(value_cfg_t *v, const cJSON *obj,
                                      bool strict, const char **err)
{
    if (err) *err = NULL;
    if (!v) return false;
    if (!cJSON_IsObject(obj)) {
        VC_FAIL("value config must be a JSON object");
        return true;
    }

    const cJSON *it;
    double d;

    // ---- formatting
    json_get_str(obj, "prefix", v->prefix, sizeof(v->prefix));
    json_get_str(obj, "suffix", v->suffix, sizeof(v->suffix));
    if ((it = cJSON_GetObjectItem(obj, "decimals")) != NULL) {
        if (vc_num(it, -1, 6, strict, &d)) v->decimals = (int8_t)d;
        else VC_FAIL("decimals must be -1..6");
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(obj, "thousands"))) {
        v->thousands = it->valuestring[0];   // "" clears the separator
    }
    if ((it = cJSON_GetObjectItem(obj, "multiplier")) != NULL) {
        if (cJSON_IsNumber(it) && it->valuedouble != 0.0 &&
            isfinite(it->valuedouble)) {
            v->multiplier = it->valuedouble;
        } else {
            VC_FAIL("multiplier must be a non-zero number");
        }
    }
    if ((it = cJSON_GetObjectItem(obj, "scale")) != NULL) {
        bool found = false;
        if (cJSON_IsString(it)) {
            for (unsigned i = 0; i < SCALE_COUNT; i++) {
                if (strcmp(it->valuestring, SCALE_NAMES[i]) == 0) {
                    v->scale = (value_scale_t)i;
                    found = true;
                    break;
                }
            }
        }
        if (!found) VC_FAIL("scale must be none|auto|k|M|B");
    }
    json_get_str(obj, "scale_letters", v->scale_letters, sizeof(v->scale_letters));
    if ((it = cJSON_GetObjectItem(obj, "dec_sep")) != NULL) {
        if (cJSON_IsString(it) && (strcmp(it->valuestring, ".") == 0 ||
                                   strcmp(it->valuestring, ",") == 0)) {
            v->dec_sep = it->valuestring[0];
        } else {
            VC_FAIL("dec_sep must be . or ,");
        }
    }
    if (cJSON_IsBool(it = cJSON_GetObjectItem(obj, "plus_sign"))) {
        v->plus_sign = cJSON_IsTrue(it);
    }

    // ---- legacy staleness keys (<= 2.11.0). Mapped first so the fb_* keys
    // below win when a body carries both. Each legacy key only moves the
    // fields it stands for; the others keep the current config.
    const cJSON *lh  = cJSON_GetObjectItem(obj, "stale_hours");
    const cJSON *la  = cJSON_GetObjectItem(obj, "stale_action");
    const cJSON *lst = cJSON_GetObjectItem(obj, "stale_text");
    const cJSON *ldt = cJSON_GetObjectItem(obj, "default_text");
    bool has_h = false, has_a = false;
    bool has_st = cJSON_IsString(lst), has_dt = cJSON_IsString(ldt);
    uint16_t hours = (uint16_t)(v->fb.after_min / 60);
    int act = legacy_act_of(&v->fb);
    if (lh) {
        if (vc_num(lh, 0, 0xFFFF, strict, &d)) {
            hours = (uint16_t)d;
            has_h = true;
        } else {
            VC_FAIL("stale_hours must be 0..65535");
        }
    }
    if (la) {
        if (cJSON_IsString(la)) {
            for (unsigned i = 0; i < LEGACY_ACT_COUNT; i++) {
                if (strcmp(la->valuestring, LEGACY_ACT_NAMES[i]) == 0) {
                    act = (int)i;
                    has_a = true;
                    break;
                }
            }
        }
        if (!has_a) VC_FAIL("stale_action must be none|dim|marker|text");
    }
    if (has_h || has_a || has_st || has_dt) {
        // Neither text key: hand the current text through unchanged.
        const char *st = has_st ? lst->valuestring
                                : (has_dt ? "" : v->fb.text);
        const char *dt = has_dt ? ldt->valuestring : "";
        fallback_cfg_t m;
        fb_migrate_legacy(&m, hours, act, st, dt);
        if (has_st || has_dt) memcpy(v->fb.text, m.text, sizeof(v->fb.text));
        if (has_a) v->fb.mode = m.mode;
        // after_min follows stale_hours, and drops to 0 for action none.
        if (has_h || (has_a && act == 0)) v->fb.after_min = m.after_min;
    }

    // ---- fallback
    json_get_str(obj, "fb_text", v->fb.text, sizeof(v->fb.text));
    if ((it = cJSON_GetObjectItem(obj, "fb_mode")) != NULL) {
        fb_mode_t m;
        if (cJSON_IsString(it) && fb_mode_from_str(it->valuestring, &m)) {
            v->fb.mode = m;
        } else {
            VC_FAIL("fb_mode must be text|marker|dim");
        }
    }
    if ((it = cJSON_GetObjectItem(obj, "fb_after_min")) != NULL) {
        if (vc_num(it, 0, FB_AFTER_MIN_MAX, strict, &d)) {
            v->fb.after_min = (uint16_t)d;
        } else {
            VC_FAIL("fb_after_min must be 0..43200");
        }
    }
    if ((it = cJSON_GetObjectItem(obj, "fb_poll_failures")) != NULL) {
        if (vc_num(it, 0, 255, strict, &d)) v->fb.poll_failures = (uint8_t)d;
        else VC_FAIL("fb_poll_failures must be 0..255");
    }
    if (cJSON_IsBool(it = cJSON_GetObjectItem(obj, "fb_on_mqtt_loss"))) {
        v->fb.on_mqtt_loss = cJSON_IsTrue(it);
    }

    // ---- cross-field
    if (!valuefmt_cfg_valid(v)) {
        VC_FAIL("thousands and decimal separator must differ");
        // Lenient: a <= 2.11.0 config could pair "." thousands with the then
        // fixed "." decimal point. Read that as the European convention.
        v->dec_sep = (v->thousands == '.') ? ',' : '.';
    }
    return true;
}

void integrations_value_cfg_to_json(const value_cfg_t *v, cJSON *obj,
                                    bool legacy)
{
    if (!v || !obj) return;
    cJSON_AddStringToObject(obj, "prefix", v->prefix);
    cJSON_AddStringToObject(obj, "suffix", v->suffix);
    cJSON_AddNumberToObject(obj, "decimals", v->decimals);
    char thou[2] = {v->thousands, '\0'};
    cJSON_AddStringToObject(obj, "thousands", thou);
    cJSON_AddNumberToObject(obj, "multiplier",
                            v->multiplier == 0.0 ? 1.0 : v->multiplier);
    unsigned sc = (unsigned)v->scale;
    cJSON_AddStringToObject(obj, "scale",
                            SCALE_NAMES[sc < SCALE_COUNT ? sc : 0]);
    cJSON_AddStringToObject(obj, "scale_letters", v->scale_letters);
    cJSON_AddStringToObject(obj, "dec_sep", v->dec_sep == ',' ? "," : ".");
    cJSON_AddBoolToObject(obj, "plus_sign", v->plus_sign);
    cJSON_AddStringToObject(obj, "fb_text", v->fb.text);
    cJSON_AddStringToObject(obj, "fb_mode", fb_mode_str(v->fb.mode));
    cJSON_AddNumberToObject(obj, "fb_after_min", v->fb.after_min);
    cJSON_AddNumberToObject(obj, "fb_poll_failures", v->fb.poll_failures);
    cJSON_AddBoolToObject(obj, "fb_on_mqtt_loss", v->fb.on_mqtt_loss);
    if (legacy) {
        // Read-only view for pre-2.12 clients, derived from the new config.
        cJSON_AddNumberToObject(obj, "stale_hours", v->fb.after_min / 60);
        cJSON_AddStringToObject(obj, "stale_action",
                                LEGACY_ACT_NAMES[legacy_act_of(&v->fb)]);
        cJSON_AddStringToObject(obj, "stale_text", v->fb.text);
        cJSON_AddStringToObject(obj, "default_text", v->fb.text);
    }
}

// Fill out->device_name from the single stored name, migrating a pre-2.2 blob
// value on the way: that value was only a webhook label, and it becomes the
// device name in all three roles. Only a name the user actually chose is
// adopted - one that resolves to the default means this device was never named,
// so the setup-AP SSID stays the historical one and the hostname the default.
static void resolve_device_name(integrations_cfg_t *out)
{
    char stored[DEVNAME_MAX + 1];
    settings_load_device_name(stored, sizeof(stored));

    if (stored[0] == '\0' && out->device_name[0] != '\0') {
        char cleaned[DEVNAME_MAX + 1];
        // The old field allowed anything (spaces, 32 chars); the name is now a
        // hostname and an SSID, so it goes through the lenient pass. A value
        // that cleans to the default (say "led display") means the device was
        // never really named, so it stays unset - the same rule
        // integrations_set_device_name() applies.
        if (devname_clean(out->device_name, cleaned, sizeof(cleaned)) &&
            strcmp(cleaned, DEVICE_NAME_DEFAULT) != 0 &&
            settings_save_device_name(cleaned) == ESP_OK) {
            snprintf(stored, sizeof(stored), "%s", cleaned);
            // WiFi came up before this ran, so hand the name over now instead
            // of waiting for the next boot.
            wifi_manager_set_device_name(cleaned);
            ESP_LOGI(TAG, "adopted '%s' as the device name", cleaned);
        }
    }
    snprintf(out->device_name, sizeof(out->device_name), "%s",
             devname_hostname(stored));
}

static void load_integr(integrations_cfg_t *out)
{
    cfg_defaults(out);

    char buf[SETTINGS_BLOB_MAX];
    if (settings_load_blob(BLOB_INTEGR, buf, sizeof(buf), NULL) != ESP_OK) {
        resolve_device_name(out);   // never written: defaults + the stored name
        return;
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "integr blob unparseable, using defaults");
        resolve_device_name(out);
        return;
    }

    json_get_str(root, "api_key",     out->api_key,     sizeof(out->api_key));
    json_get_str(root, "webhook_url", out->webhook_url, sizeof(out->webhook_url));
    json_get_str(root, "device_name", out->device_name, sizeof(out->device_name));
    const cJSON *ev = cJSON_GetObjectItem(root, "webhook_events");
    if (cJSON_IsNumber(ev)) out->webhook_events = (uint32_t)ev->valuedouble;

    json_get_str(root, "ui_user", out->ui_user, sizeof(out->ui_user));
    json_get_str(root, "ui_pass", out->ui_pass, sizeof(out->ui_pass));
    const cJSON *ua = cJSON_GetObjectItem(root, "ui_auth");
    // A blob written before 2.8.0 has no ui_auth, which must read as off:
    // an update is not allowed to lock anyone out of their own display.
    out->ui_auth = cJSON_IsTrue(ua) && out->ui_pass[0] != '\0';

    // Webhook auth and heartbeat (2.12.0): absent in older blobs = off.
    json_get_str(root, "webhook_hdr_name", out->webhook_hdr_name,
                 sizeof(out->webhook_hdr_name));
    json_get_str(root, "webhook_hdr_value", out->webhook_hdr_value,
                 sizeof(out->webhook_hdr_value));
    const cJSON *hb = cJSON_GetObjectItem(root, "heartbeat_min");
    if (cJSON_IsNumber(hb)) {
        int m = hb->valueint;
        out->heartbeat_min = (uint16_t)(m < 0 ? 0 : (m > INTEGR_HEARTBEAT_MAX
                                                     ? INTEGR_HEARTBEAT_MAX : m));
    }

    cJSON_Delete(root);
    resolve_device_name(out);
}

static esp_err_t save_valuecfg(const value_cfg_t *in);

static void load_valuecfg(value_cfg_t *out)
{
    value_cfg_defaults(out);

    char buf[SETTINGS_BLOB_MAX];
    if (settings_load_blob(BLOB_VALUECFG, buf, sizeof(buf), NULL) != ESP_OK) {
        return;
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "valuecfg blob unparseable, using defaults");
        return;
    }

    // Lenient: a stored value out of today's range is clamped, never a reason
    // to drop the whole config. A v1 blob (<= 2.11.0) carries only the legacy
    // staleness keys, which the parser maps through fb_migrate_legacy.
    const cJSON *ver = cJSON_GetObjectItem(root, "v");
    int version = cJSON_IsNumber(ver) ? ver->valueint : 1;
    integrations_value_cfg_from_json(out, root, false, NULL);
    cJSON_Delete(root);

    if (version < VALUECFG_VERSION) {
        esp_err_t err = save_valuecfg(out);
        ESP_LOGI(TAG, "valuecfg v%d migrated to v%d%s", version,
                 VALUECFG_VERSION, err == ESP_OK ? "" : " (re-save failed)");
    }
}

static esp_err_t save_integr(const integrations_cfg_t *in)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "api_key", in->api_key);
    cJSON_AddStringToObject(root, "webhook_url", in->webhook_url);
    cJSON_AddNumberToObject(root, "webhook_events", in->webhook_events);
    cJSON_AddStringToObject(root, "device_name", in->device_name);
    cJSON_AddBoolToObject(root, "ui_auth", in->ui_auth);
    cJSON_AddStringToObject(root, "ui_user", in->ui_user);
    cJSON_AddStringToObject(root, "ui_pass", in->ui_pass);
    cJSON_AddStringToObject(root, "webhook_hdr_name", in->webhook_hdr_name);
    cJSON_AddStringToObject(root, "webhook_hdr_value", in->webhook_hdr_value);
    cJSON_AddNumberToObject(root, "heartbeat_min", in->heartbeat_min);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    esp_err_t err = settings_save_blob(BLOB_INTEGR, json);
    cJSON_free(json);
    return err;
}

static esp_err_t save_valuecfg(const value_cfg_t *in)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "v", VALUECFG_VERSION);
    integrations_value_cfg_to_json(in, root, false);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    esp_err_t err = settings_save_blob(BLOB_VALUECFG, json);
    cJSON_free(json);
    return err;
}

// ---- public API ------------------------------------------------------------

esp_err_t integrations_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    // Re-init is legal (config import calls it to recache after blob writes),
    // so load into locals and swap under the lock.
    integrations_cfg_t cfg;
    value_cfg_t vcfg;
    load_integr(&cfg);
    load_valuecfg(&vcfg);

    lock();
    s_cfg       = cfg;
    s_value_cfg = vcfg;
    unlock();

    // Never log the key or URL themselves (DESIGN.md section 10).
    ESP_LOGI(TAG, "init: auth=%s webhook=%s events=0x%02x device='%s' "
                  "fallback=%umin/%s heartbeat=%umin",
             cfg.api_key[0] ? "on" : "off",
             cfg.webhook_url[0] ? "set" : "none",
             (unsigned)cfg.webhook_events, cfg.device_name,
             (unsigned)vcfg.fb.after_min, fb_mode_str(vcfg.fb.mode),
             (unsigned)cfg.heartbeat_min);
    return ESP_OK;
}

void integrations_get(integrations_cfg_t *out)
{
    if (!out) return;
    lock();
    *out = s_cfg;
    unlock();
}

// The one place a device name is written. Stores it under the "dname" key,
// refreshes the cache the webhook worker reads, and hands it to the WiFi
// manager for the hostname and the setup-AP SSID. `name` is cleaned rather
// than rejected here - the API handlers do the strict validation and report
// the rule; this path also serves config imports, whose files may predate it.
//
// A name equal to the default is stored as "unset", so an untouched device
// keeps the historical hostname and setup-AP SSID.
esp_err_t integrations_set_device_name(const char *name)
{
    char cleaned[DEVNAME_MAX + 1];
    if (!devname_clean(name, cleaned, sizeof(cleaned))) cleaned[0] = '\0';
    if (strcmp(cleaned, DEVICE_NAME_DEFAULT) == 0) cleaned[0] = '\0';

    esp_err_t err = settings_save_device_name(cleaned);
    if (err != ESP_OK) return err;

    integrations_cfg_t cfg;
    integrations_get(&cfg);
    snprintf(cfg.device_name, sizeof(cfg.device_name), "%s",
             devname_hostname(cleaned));
    err = save_integr(&cfg);   // keep the rollback mirror in step
    if (err != ESP_OK) return err;

    lock();
    s_cfg = cfg;
    unlock();

    wifi_manager_set_device_name(cleaned);
    return ESP_OK;
}

esp_err_t integrations_set(const integrations_cfg_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;

    // Defensive copy that guarantees NUL termination whatever the caller
    // stuffed into the fixed arrays.
    integrations_cfg_t cfg = *in;
    cfg.api_key[INTEGR_API_KEY_MAX]  = '\0';
    cfg.webhook_url[INTEGR_URL_MAX]  = '\0';
    cfg.device_name[INTEGR_NAME_MAX] = '\0';
    cfg.webhook_hdr_name[INTEGR_HDR_NAME_MAX] = '\0';
    cfg.webhook_hdr_value[INTEGR_SECRET_MAX]  = '\0';
    if (cfg.heartbeat_min > INTEGR_HEARTBEAT_MAX) {
        cfg.heartbeat_min = INTEGR_HEARTBEAT_MAX;
    }

    // The name has its own storage and its own side effects: route it through
    // the funnel, then let the blob write below carry the resolved value.
    char current[INTEGR_NAME_MAX + 1];
    integrations_get_device_name(current, sizeof(current));
    if (strcmp(cfg.device_name, current) != 0) {
        esp_err_t nerr = integrations_set_device_name(cfg.device_name);
        if (nerr != ESP_OK) return nerr;
    }
    integrations_get_device_name(cfg.device_name, sizeof(cfg.device_name));

    esp_err_t err = save_integr(&cfg);
    if (err != ESP_OK) return err;

    lock();
    s_cfg = cfg;
    unlock();
    return ESP_OK;
}

void integrations_get_device_name(char *buf, size_t bufsize)
{
    if (!buf || bufsize == 0) return;
    lock();
    snprintf(buf, bufsize, "%s", s_cfg.device_name);
    unlock();
}

void integrations_get_value_cfg(value_cfg_t *out)
{
    if (!out) return;
    lock();
    *out = s_value_cfg;
    unlock();
}

esp_err_t integrations_set_value_cfg(const value_cfg_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;

    value_cfg_t cfg = *in;
    cfg.prefix[VALUE_AFFIX_MAX]      = '\0';
    cfg.suffix[VALUE_AFFIX_MAX]      = '\0';
    cfg.scale_letters[VALUE_LETTERS_MAX] = '\0';
    cfg.fb.text[FB_TEXT_MAX]         = '\0';
    if (cfg.decimals < -1) cfg.decimals = -1;
    if (cfg.decimals > 6)  cfg.decimals = 6;
    if (cfg.multiplier == 0.0 || !isfinite(cfg.multiplier)) cfg.multiplier = 1.0;
    if ((unsigned)cfg.scale > VALUE_SCALE_B) cfg.scale = VALUE_SCALE_NONE;
    if ((unsigned)cfg.fb.mode > FB_MODE_DIM) cfg.fb.mode = FB_MODE_TEXT;
    if (cfg.fb.after_min > FB_AFTER_MIN_MAX) cfg.fb.after_min = FB_AFTER_MIN_MAX;

    esp_err_t err = save_valuecfg(&cfg);
    if (err != ESP_OK) return err;

    lock();
    s_value_cfg = cfg;
    unlock();
    return ESP_OK;
}

void integrations_touch_value(void)
{
    // 64-bit store is not atomic on this core: share the config mutex.
    lock();
    s_last_value_us = esp_timer_get_time();
    unlock();
}

int64_t integrations_value_age_s(void)
{
    lock();
    int64_t last = s_last_value_us;
    unlock();
    if (last < 0) return -1;
    return (esp_timer_get_time() - last) / 1000000;
}

void integrations_set_last_value(const char *text)
{
    if (!text) return;
    lock();
    snprintf(s_last_value_text, sizeof(s_last_value_text), "%s", text);
    s_last_value_set = true;
    unlock();
}

bool integrations_get_last_value(char *buf, size_t bufsize)
{
    if (!buf || bufsize == 0) return false;
    lock();
    bool set = s_last_value_set;
    snprintf(buf, bufsize, "%s", s_last_value_text);
    unlock();
    return set;
}
