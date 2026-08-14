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

#include "devname.h"
#include "settings.h"
#include "wifi_manager.h"

static const char *TAG = "integr";

#define BLOB_INTEGR   "integr"
#define BLOB_VALUECFG "valuecfg"

#define DEVICE_NAME_DEFAULT DEVNAME_DEFAULT_HOST

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

    cJSON_Delete(root);
    resolve_device_name(out);
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
