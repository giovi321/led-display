// Integration settings (see integrations.h): RAM cache in front of two JSON
// blobs in the "cfg" NVS namespace. Reads must stay cheap and flash-free
// because web_auth_ok() calls integrations_get() on every mutating request
// and the scheduler polls the value config every 30 s.
//
// Blob shapes (versioned for forward migration, DESIGN.md section 4):
//   "integr"   {"v":1,"api_key":"..","webhook_url":"..",
//               "webhook_events":<mask>,"device_name":".."}
//   "valuecfg" {"v":1,"prefix":"..","suffix":"..","decimals":-1,
//               "thousands":"","stale_hours":0,"stale_action":"none",
//               "stale_text":".."}

#include "integrations.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "settings.h"

static const char *TAG = "integr";

#define BLOB_INTEGR   "integr"
#define BLOB_VALUECFG "valuecfg"

#define DEVICE_NAME_DEFAULT "led-display"

// Static initialisers double as the pre-init defaults: web_auth_ok() may run
// before integrations_init() finishes and must see "no key = open".
static SemaphoreHandle_t  s_lock;
static integrations_cfg_t s_cfg       = {.device_name = DEVICE_NAME_DEFAULT};
static value_cfg_t        s_value_cfg = {.decimals = -1};
static int64_t            s_last_value_us = -1;   // -1 = no value yet

// stale_action_t <-> blob/API string. Kept as strings in the blob so the
// config export stays self-describing.
static const char *STALE_NAMES[] = {"none", "dim", "marker", "text"};

static const char *stale_name(stale_action_t a)
{
    if ((unsigned)a >= sizeof(STALE_NAMES) / sizeof(STALE_NAMES[0])) {
        return STALE_NAMES[STALE_ACT_NONE];
    }
    return STALE_NAMES[a];
}

static stale_action_t stale_from_name(const char *name)
{
    for (unsigned i = 0; i < sizeof(STALE_NAMES) / sizeof(STALE_NAMES[0]); i++) {
        if (strcmp(name, STALE_NAMES[i]) == 0) return (stale_action_t)i;
    }
    return STALE_ACT_NONE;
}

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
    v->decimals     = -1;              // leave values as sent
    v->stale_action = STALE_ACT_NONE;
}

// ---- blob (de)serialisation ------------------------------------------------

static void json_get_str(const cJSON *root, const char *key,
                         char *dst, size_t dstsz)
{
    const cJSON *it = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(it)) snprintf(dst, dstsz, "%s", it->valuestring);
}

static void load_integr(integrations_cfg_t *out)
{
    cfg_defaults(out);

    char buf[SETTINGS_BLOB_MAX];
    if (settings_load_blob(BLOB_INTEGR, buf, sizeof(buf), NULL) != ESP_OK) {
        return;   // never written: defaults
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "integr blob unparseable, using defaults");
        return;
    }

    json_get_str(root, "api_key",     out->api_key,     sizeof(out->api_key));
    json_get_str(root, "webhook_url", out->webhook_url, sizeof(out->webhook_url));
    json_get_str(root, "device_name", out->device_name, sizeof(out->device_name));
    if (out->device_name[0] == '\0') {
        snprintf(out->device_name, sizeof(out->device_name), DEVICE_NAME_DEFAULT);
    }
    const cJSON *ev = cJSON_GetObjectItem(root, "webhook_events");
    if (cJSON_IsNumber(ev)) out->webhook_events = (uint32_t)ev->valuedouble;

    cJSON_Delete(root);
}

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

    json_get_str(root, "prefix",     out->prefix,     sizeof(out->prefix));
    json_get_str(root, "suffix",     out->suffix,     sizeof(out->suffix));
    json_get_str(root, "stale_text", out->stale_text, sizeof(out->stale_text));

    const cJSON *dec = cJSON_GetObjectItem(root, "decimals");
    if (cJSON_IsNumber(dec)) {
        int d = dec->valueint;
        out->decimals = (int8_t)(d < -1 ? -1 : (d > 9 ? 9 : d));
    }
    const cJSON *th = cJSON_GetObjectItem(root, "thousands");
    if (cJSON_IsString(th)) out->thousands = th->valuestring[0];   // "" -> 0

    const cJSON *sh = cJSON_GetObjectItem(root, "stale_hours");
    if (cJSON_IsNumber(sh)) {
        int h = sh->valueint;
        out->stale_hours = (uint16_t)(h < 0 ? 0 : (h > 0xFFFF ? 0xFFFF : h));
    }
    const cJSON *sa = cJSON_GetObjectItem(root, "stale_action");
    if (cJSON_IsString(sa)) out->stale_action = stale_from_name(sa->valuestring);

    cJSON_Delete(root);
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
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddStringToObject(root, "prefix", in->prefix);
    cJSON_AddStringToObject(root, "suffix", in->suffix);
    cJSON_AddNumberToObject(root, "decimals", in->decimals);
    char thou[2] = {in->thousands, '\0'};
    cJSON_AddStringToObject(root, "thousands", thou);
    cJSON_AddNumberToObject(root, "stale_hours", in->stale_hours);
    cJSON_AddStringToObject(root, "stale_action", stale_name(in->stale_action));
    cJSON_AddStringToObject(root, "stale_text", in->stale_text);

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
                  "stale=%uh/%s",
             cfg.api_key[0] ? "on" : "off",
             cfg.webhook_url[0] ? "set" : "none",
             (unsigned)cfg.webhook_events, cfg.device_name,
             (unsigned)vcfg.stale_hours, stale_name(vcfg.stale_action));
    return ESP_OK;
}

void integrations_get(integrations_cfg_t *out)
{
    if (!out) return;
    lock();
    *out = s_cfg;
    unlock();
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
    if (cfg.device_name[0] == '\0') {
        snprintf(cfg.device_name, sizeof(cfg.device_name), DEVICE_NAME_DEFAULT);
    }

    esp_err_t err = save_integr(&cfg);
    if (err != ESP_OK) return err;

    lock();
    s_cfg = cfg;
    unlock();
    return ESP_OK;
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
    cfg.prefix[VALUE_AFFIX_MAX]    = '\0';
    cfg.suffix[VALUE_AFFIX_MAX]    = '\0';
    cfg.stale_text[VALUE_TEXT_MAX] = '\0';
    if (cfg.decimals < -1) cfg.decimals = -1;
    if (cfg.decimals > 9)  cfg.decimals = 9;

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
