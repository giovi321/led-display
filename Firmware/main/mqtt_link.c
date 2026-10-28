// MQTT client glue (see mqtt_link.h). The real implementation (behind
// CONFIG_LEDDISP_MQTT) drives esp-mqtt; the no-op branch at the bottom stays
// as the permanent fallback so callers never need #ifdefs.
//
// Blob shape (versioned for forward migration, DESIGN.md section 4):
//   "mqtt" {"v":1,"enabled":false,"uri":"mqtt://host:1883","username":"..",
//           "password":"..","base_topic":"..","ha_discovery":false,
//           "ha_prefix":"homeassistant"}
//
// Topic layout (BASE = base_topic, or the device name when unset):
//   BASE/availability          retained "online"/"offline" (LWT)
//   BASE/state                 retained JSON, every 30 s + after commands
//   BASE/text/set              show + persist static text
//   BASE/value/set             push a value (same path as POST /api/value)
//   BASE/brightness/set        0..255 (clamped to the cap)
//   BASE/power/set             ON/OFF (blank the panel / restore content)
//   BASE/source/set            static|integration
//   BASE/reboot/set            any payload reboots

#include "mqtt_link.h"

#include <string.h>

#include "sdkconfig.h"

#if CONFIG_LEDDISP_MQTT

#include <stdio.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "api_handlers.h"
#include "display.h"
#include "fw_version.h"
#include "integrations.h"
#include "led_driver.h"
#include "scheduler.h"
#include "settings.h"
#include "value_feed.h"
#include "wifi_manager.h"

static const char *TAG = "mqtt";

#define BLOB_MQTT          "mqtt"
#define STATE_PERIOD_MS    30000
#define STATE_TASK_STACK   4096
#define STATE_TASK_PRIO    2            // below the display refresh, above idle
#define HA_PREFIX_DEFAULT  "homeassistant"

static SemaphoreHandle_t         s_lock;
static mqtt_cfg_t                s_cfg;         // persisted config (RAM cache)
static esp_mqtt_client_handle_t  s_client;
static volatile bool             s_connected;
static TaskHandle_t              s_state_task;

// esp-mqtt keeps the pointers we hand it (URI, credentials, LWT topic) for
// the client's lifetime, so they live in an "active" copy only rewritten
// while the client is down. Same pattern as wg_client.c.
static mqtt_cfg_t s_active;
static char       s_node[INTEGR_NAME_MAX + 1];              // device name
static char       s_base[MQTT_TOPIC_MAX + INTEGR_NAME_MAX + 1];
static char       s_avail_topic[sizeof(s_base) + 16];

// Tolerate pre-init callers, same pattern as integrations.c.
static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// ---- config (de)serialisation -----------------------------------------------

static void cfg_defaults(mqtt_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    snprintf(c->ha_prefix, sizeof(c->ha_prefix), HA_PREFIX_DEFAULT);
}

static void json_get_str(const cJSON *root, const char *key,
                         char *dst, size_t dstsz)
{
    const cJSON *it = cJSON_GetObjectItem(root, key);
    if (cJSON_IsString(it)) snprintf(dst, dstsz, "%s", it->valuestring);
}

static void load_cfg(mqtt_cfg_t *out)
{
    cfg_defaults(out);

    char buf[SETTINGS_BLOB_MAX];
    if (settings_load_blob(BLOB_MQTT, buf, sizeof(buf), NULL) != ESP_OK) {
        return;   // never written: defaults (disabled)
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "mqtt blob unparseable, using defaults");
        return;
    }

    const cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(en)) out->enabled = cJSON_IsTrue(en);
    const cJSON *ha = cJSON_GetObjectItem(root, "ha_discovery");
    if (cJSON_IsBool(ha)) out->ha_discovery = cJSON_IsTrue(ha);

    json_get_str(root, "uri",        out->uri,        sizeof(out->uri));
    json_get_str(root, "username",   out->username,   sizeof(out->username));
    json_get_str(root, "password",   out->password,   sizeof(out->password));
    json_get_str(root, "base_topic", out->base_topic, sizeof(out->base_topic));
    json_get_str(root, "ha_prefix",  out->ha_prefix,  sizeof(out->ha_prefix));

    cJSON_Delete(root);
}

static esp_err_t save_cfg(const mqtt_cfg_t *in)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddBoolToObject(root, "enabled", in->enabled);
    cJSON_AddStringToObject(root, "uri", in->uri);
    cJSON_AddStringToObject(root, "username", in->username);
    cJSON_AddStringToObject(root, "password", in->password);
    cJSON_AddStringToObject(root, "base_topic", in->base_topic);
    cJSON_AddBoolToObject(root, "ha_discovery", in->ha_discovery);
    cJSON_AddStringToObject(root, "ha_prefix", in->ha_prefix);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    esp_err_t err = settings_save_blob(BLOB_MQTT, json);
    cJSON_free(json);
    return err;
}

// ---- state + discovery publishing --------------------------------------------
//
// Everything the device publishes goes through esp_mqtt_client_enqueue with
// store=true, never esp_mqtt_client_publish: publish() sends synchronously in
// the CALLING task's context, and most of these run from the esp-mqtt event
// task itself (the connect handler, the post-command feedback), where a
// blocking send is exactly the deadlock the component warns about.

static void publish(const char *topic, const char *payload, int qos)
{
    esp_mqtt_client_enqueue(s_client, topic, payload, 0, qos, 1 /* retain */,
                            true /* store */);
}

static void publish_state_locked(void)
{
    if (!s_client || !s_connected) return;

    char text[DISPLAY_TEXT_MAX];
    display_get_state(text, sizeof(text), NULL, NULL, NULL, NULL);

    settings_t cfg;
    settings_load(&cfg);

    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);

    cJSON *r = cJSON_CreateObject();
    if (!r) return;
    cJSON_AddStringToObject(r, "power", text[0] != '\0' ? "ON" : "OFF");
    cJSON_AddNumberToObject(r, "brightness", cfg.brightness);
    cJSON_AddStringToObject(r, "text", text);
    cJSON_AddStringToObject(r, "source",
                            cfg.source == DISPLAY_SRC_INTEGRATION
                                ? "integration" : "static");
    cJSON_AddNumberToObject(r, "value_age_s",
                            (double)integrations_value_age_s());
    cJSON_AddNumberToObject(r, "rssi", w.rssi);
    cJSON_AddStringToObject(r, "ip", w.ip);
    cJSON_AddStringToObject(r, "fw", FW_VERSION);
    cJSON_AddNumberToObject(r, "uptime_s",
                            (double)(esp_timer_get_time() / 1000000));

    char *json = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (!json) return;

    char topic[sizeof(s_base) + 8];
    snprintf(topic, sizeof(topic), "%s/state", s_base);
    publish(topic, json, 0);
    cJSON_free(json);
}

// Shared "device" object that groups every entity under one HA device page.
static cJSON *ha_device_json(const char *ip)
{
    cJSON *d = cJSON_CreateObject();
    cJSON *ids = cJSON_AddArrayToObject(d, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(s_node));
    cJSON_AddStringToObject(d, "name", s_node);
    cJSON_AddStringToObject(d, "model", "led-display");
    cJSON_AddStringToObject(d, "manufacturer", "giovi321");
    cJSON_AddStringToObject(d, "sw_version", FW_VERSION);
    if (ip && ip[0]) {
        char url[48];
        snprintf(url, sizeof(url), "http://%s/", ip);
        cJSON_AddStringToObject(d, "configuration_url", url);
    }
    return d;
}

// Start a discovery config with the fields every entity shares. `object_id`
// doubles as the unique_id suffix.
static cJSON *ha_entity_json(const char *display_name, const char *object_id,
                             const char *ip)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "name", display_name);
    char uid[INTEGR_NAME_MAX + 24];
    snprintf(uid, sizeof(uid), "%s_%s", s_node, object_id);
    cJSON_AddStringToObject(r, "unique_id", uid);
    cJSON_AddStringToObject(r, "availability_topic", s_avail_topic);
    cJSON_AddItemToObject(r, "device", ha_device_json(ip));
    return r;
}

static void ha_publish(const char *component, const char *object_id,
                       cJSON *config /* consumed; NULL = remove */)
{
    char topic[MQTT_PREFIX_MAX + INTEGR_NAME_MAX + 48];
    snprintf(topic, sizeof(topic), "%s/%s/%s/%s/config",
             s_active.ha_prefix[0] ? s_active.ha_prefix : HA_PREFIX_DEFAULT,
             component, s_node, object_id);

    if (!config) {
        // Empty retained payload deletes the discovery entry.
        publish(topic, "", 0);
        return;
    }
    char *json = cJSON_PrintUnformatted(config);
    cJSON_Delete(config);
    if (!json) return;
    publish(topic, json, 0);
    cJSON_free(json);
}

static void topic_str(cJSON *r, const char *key, const char *suffix)
{
    char t[sizeof(s_base) + 24];
    snprintf(t, sizeof(t), "%s/%s", s_base, suffix);
    cJSON_AddStringToObject(r, key, t);
}

// The entity set is deliberately small: panel on/off + brightness as a
// light, the static text, the content source, reboot, and two diagnostic
// sensors. Everything else stays web-UI-only.
static void publish_discovery(bool remove)
{
    if (!s_client || !s_connected) return;

    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);
    const char *ip = w.ip;

    if (remove) {
        ha_publish("light",  "display",   NULL);
        ha_publish("text",   "text",      NULL);
        ha_publish("select", "source",    NULL);
        ha_publish("button", "reboot",    NULL);
        ha_publish("sensor", "rssi",      NULL);
        ha_publish("sensor", "value_age", NULL);
        return;
    }

    cJSON *r = ha_entity_json("Display", "display", ip);
    topic_str(r, "state_topic", "state");
    cJSON_AddStringToObject(r, "state_value_template", "{{ value_json.power }}");
    topic_str(r, "command_topic", "power/set");
    cJSON_AddStringToObject(r, "payload_on", "ON");
    cJSON_AddStringToObject(r, "payload_off", "OFF");
    topic_str(r, "brightness_state_topic", "state");
    cJSON_AddStringToObject(r, "brightness_value_template",
                            "{{ value_json.brightness }}");
    topic_str(r, "brightness_command_topic", "brightness/set");
    cJSON_AddNumberToObject(r, "brightness_scale", 255);
    ha_publish("light", "display", r);

    r = ha_entity_json("Text", "text", ip);
    topic_str(r, "state_topic", "state");
    cJSON_AddStringToObject(r, "value_template", "{{ value_json.text }}");
    topic_str(r, "command_topic", "text/set");
    cJSON_AddNumberToObject(r, "max", DISPLAY_TEXT_MAX - 1);
    ha_publish("text", "text", r);

    r = ha_entity_json("Content source", "source", ip);
    topic_str(r, "state_topic", "state");
    cJSON_AddStringToObject(r, "value_template", "{{ value_json.source }}");
    topic_str(r, "command_topic", "source/set");
    cJSON *opts = cJSON_AddArrayToObject(r, "options");
    cJSON_AddItemToArray(opts, cJSON_CreateString("static"));
    cJSON_AddItemToArray(opts, cJSON_CreateString("integration"));
    cJSON_AddStringToObject(r, "entity_category", "config");
    ha_publish("select", "source", r);

    r = ha_entity_json("Reboot", "reboot", ip);
    topic_str(r, "command_topic", "reboot/set");
    cJSON_AddStringToObject(r, "entity_category", "config");
    cJSON_AddStringToObject(r, "device_class", "restart");
    ha_publish("button", "reboot", r);

    r = ha_entity_json("WiFi RSSI", "rssi", ip);
    topic_str(r, "state_topic", "state");
    cJSON_AddStringToObject(r, "value_template", "{{ value_json.rssi }}");
    cJSON_AddStringToObject(r, "unit_of_measurement", "dBm");
    cJSON_AddStringToObject(r, "device_class", "signal_strength");
    cJSON_AddStringToObject(r, "entity_category", "diagnostic");
    ha_publish("sensor", "rssi", r);

    r = ha_entity_json("Value age", "value_age", ip);
    topic_str(r, "state_topic", "state");
    cJSON_AddStringToObject(r, "value_template", "{{ value_json.value_age_s }}");
    cJSON_AddStringToObject(r, "unit_of_measurement", "s");
    cJSON_AddStringToObject(r, "entity_category", "diagnostic");
    ha_publish("sensor", "value_age", r);
}

// ---- inbound commands ---------------------------------------------------------

static void handle_text(const char *payload)
{
    // Same semantics as POST /api/display/text with only "text": render in
    // the current style and persist as the static text.
    display_color_t col;
    uint8_t mode, speed;
    display_get_state(NULL, 0, &col, &mode, &speed, NULL);
    display_show_text(payload, col, mode, speed, 0);
    settings_save_text(payload, NULL, mode, speed, 0);
}

static void handle_value(const char *payload)
{
    // Same path as POST /api/value: format, remember, count as fresh, clear
    // the fallback, render only while the display source is "integration".
    // Runs without s_lock: value_feed may publish the state itself.
    value_feed_ingest(payload, VALUE_SRC_MQTT, NULL, 0);
}

static void handle_brightness(const char *payload)
{
    char *end = NULL;
    long v = strtol(payload, &end, 10);
    if (end == payload || v < 0 || v > 255) {
        ESP_LOGW(TAG, "brightness/set: bad payload");
        return;
    }
    uint8_t cap = led_driver_get_brightness_cap();
    if (v > cap) v = cap;
    settings_save_brightness((uint8_t)v);
    scheduler_reapply_brightness();
}

static void handle_power(const char *payload)
{
    if (!strcasecmp(payload, "ON") || !strcmp(payload, "1") ||
        !strcasecmp(payload, "true")) {
        api_display_apply_source();
    } else if (!strcasecmp(payload, "OFF") || !strcmp(payload, "0") ||
               !strcasecmp(payload, "false")) {
        // RAM only: the stored text/value survives, power ON restores it.
        display_clear();
    } else {
        ESP_LOGW(TAG, "power/set: bad payload");
    }
}

static void handle_source(const char *payload)
{
    display_source_t src;
    if      (!strcmp(payload, "static"))      src = DISPLAY_SRC_STATIC;
    else if (!strcmp(payload, "integration")) src = DISPLAY_SRC_INTEGRATION;
    else {
        ESP_LOGW(TAG, "source/set: bad payload");
        return;
    }
    settings_save_source(src);
    api_display_apply_source();
}

static void handle_reboot(void)
{
    ESP_LOGI(TAG, "reboot requested over MQTT");
    // Let the broker see the ack/state before the link dies.
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void dispatch(const char *suffix, const char *payload)
{
    if      (!strcmp(suffix, "text/set"))       handle_text(payload);
    else if (!strcmp(suffix, "value/set"))      handle_value(payload);
    else if (!strcmp(suffix, "brightness/set")) handle_brightness(payload);
    else if (!strcmp(suffix, "power/set"))      handle_power(payload);
    else if (!strcmp(suffix, "source/set"))     handle_source(payload);
    else if (!strcmp(suffix, "reboot/set"))     { handle_reboot(); return; }
    else return;   // not ours

    // Immediate feedback so HA sliders/toggles do not snap back.
    lock();
    publish_state_locked();
    unlock();
}

// ---- esp-mqtt event handler ----------------------------------------------------

static void subscribe_all(void)
{
    static const char *SUFFIXES[] = {
        "text/set", "value/set", "brightness/set", "power/set",
        "source/set", "reboot/set",
    };
    for (unsigned i = 0; i < sizeof(SUFFIXES) / sizeof(SUFFIXES[0]); i++) {
        char t[sizeof(s_base) + 24];
        snprintf(t, sizeof(t), "%s/%s", s_base, SUFFIXES[i]);
        esp_mqtt_client_subscribe(s_client, t, 0);
    }
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t event_id,
                       void *event_data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "connected");
            s_connected = true;
            value_feed_mqtt_state(true);
            subscribe_all();
            publish(s_avail_topic, "online", 1);
            lock();
            if (s_active.ha_discovery) publish_discovery(false);
            publish_state_locked();
            unlock();
            break;

        case MQTT_EVENT_DISCONNECTED:
            if (s_connected) ESP_LOGW(TAG, "disconnected");
            s_connected = false;
            // Starts the MQTT-loss grace only on the first event of an outage.
            value_feed_mqtt_state(false);
            break;

        case MQTT_EVENT_DATA: {
            // Commands are short; a payload that arrives fragmented (bigger
            // than the client's RX buffer) is not one of ours.
            if (ev->topic_len == 0 || ev->data_len != ev->total_data_len) break;

            char topic[sizeof(s_base) + 24];
            if ((size_t)ev->topic_len >= sizeof(topic)) break;
            memcpy(topic, ev->topic, ev->topic_len);
            topic[ev->topic_len] = '\0';

            size_t blen = strlen(s_base);
            if (strncmp(topic, s_base, blen) != 0 || topic[blen] != '/') break;

            char payload[DISPLAY_TEXT_MAX];
            size_t n = (size_t)ev->data_len < sizeof(payload) - 1
                           ? (size_t)ev->data_len : sizeof(payload) - 1;
            memcpy(payload, ev->data, n);
            payload[n] = '\0';

            dispatch(topic + blen + 1, payload);
            break;
        }

        default:
            break;
    }
}

// ---- client lifecycle ----------------------------------------------------------
//
// NEVER under s_lock: esp_mqtt_client_stop waits for the event task, whose
// handlers take s_lock. Lifecycle calls come from boot init and the (httpd-
// serialised) apply path only, so s_active is stable while a client is down.

static void client_stop(void)
{
    if (!s_client) return;
    // Best effort: tell listeners we are going away on a clean stop (the LWT
    // only covers unclean drops). The synchronous publish is deliberate here
    // and safe - this path never runs in the event task - because an enqueued
    // message would be thrown away by the stop below. QoS 0 so a broker that
    // has already gone away cannot block the teardown waiting for an ack.
    if (s_connected) {
        esp_mqtt_client_publish(s_client, s_avail_topic, "offline", 0, 0, 1);
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    s_connected = false;
}

static esp_err_t client_start(void)
{
    // Freeze the strings esp-mqtt will keep pointing at.
    integrations_get_device_name(s_node, sizeof(s_node));
    if (s_active.base_topic[0] != '\0') {
        snprintf(s_base, sizeof(s_base), "%s", s_active.base_topic);
    } else {
        snprintf(s_base, sizeof(s_base), "%s", s_node);
    }
    snprintf(s_avail_topic, sizeof(s_avail_topic), "%s/availability", s_base);

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = s_active.uri,
        .credentials.client_id = s_node,
        .session.keepalive = 30,
        .session.last_will = {
            .topic  = s_avail_topic,
            .msg    = "offline",
            .qos    = 1,
            .retain = true,
        },
    };
    if (s_active.username[0]) {
        mc.credentials.username = s_active.username;
        mc.credentials.authentication.password = s_active.password;
    }
    if (!strncmp(s_active.uri, "mqtts://", 8) ||
        !strncmp(s_active.uri, "wss://", 6)) {
        mc.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }

    s_client = esp_mqtt_client_init(&mc);
    if (!s_client) return ESP_FAIL;
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, mqtt_event, NULL);
    esp_err_t err = esp_mqtt_client_start(s_client);
    if (err != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        return err;
    }
    return ESP_OK;
}

// A task rather than an esp_timer: building the state JSON reads NVS and
// allocates, which does not belong on the shared esp_timer task's small
// stack. Runs for the lifetime of the app and no-ops while disconnected.
static void state_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(STATE_PERIOD_MS));
        lock();
        publish_state_locked();
        unlock();
    }
}

// ---- public API ----------------------------------------------------------------

esp_err_t mqtt_link_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }
    if (!s_state_task) {
        if (xTaskCreate(state_task, "mqtt_state", STATE_TASK_STACK, NULL,
                        STATE_TASK_PRIO, &s_state_task) != pdPASS) {
            s_state_task = NULL;
            ESP_LOGW(TAG, "state task not created; state published on commands only");
        }
    }

    mqtt_cfg_t cfg;
    load_cfg(&cfg);

    lock();
    s_cfg    = cfg;
    s_active = cfg;
    unlock();

    // The password never appears in logs (DESIGN.md section 10).
    ESP_LOGI(TAG, "init: enabled=%d uri=%s user=%s discovery=%d",
             cfg.enabled, cfg.uri[0] ? cfg.uri : "(unset)",
             cfg.username[0] ? cfg.username : "(anonymous)",
             cfg.ha_discovery);

    // client_start() runs unlocked on purpose: no client exists yet, so
    // s_active is only touched from this task, and the event task (which
    // takes s_lock in its handlers) must never be waited on under the lock.
    if (cfg.enabled && cfg.uri[0]) {
        esp_err_t err = client_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "client start failed: %s", esp_err_to_name(err));
            value_feed_mqtt_state(false);   // enabled but no link: an outage
        }
    }
    return ESP_OK;
}

esp_err_t mqtt_link_apply(const mqtt_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    mqtt_cfg_t c = *cfg;
    c.uri[MQTT_URI_MAX]           = '\0';
    c.username[MQTT_USER_MAX]     = '\0';
    c.password[MQTT_PASS_MAX]     = '\0';
    c.base_topic[MQTT_TOPIC_MAX]  = '\0';
    c.ha_prefix[MQTT_PREFIX_MAX]  = '\0';

    if (c.enabled && !c.uri[0]) return ESP_ERR_INVALID_ARG;

    esp_err_t err = save_cfg(&c);
    if (err != ESP_OK) return err;

    // Everything below runs WITHOUT s_lock: esp_mqtt_client_stop waits for
    // the event task, whose handlers take s_lock - stopping under the lock
    // would deadlock. httpd serialises the API handlers, so no second apply
    // races this one; the lock below only guards the readers.

    // Discovery switched off: withdraw the retained configs while the old
    // client is still connected, or HA keeps ghost entities forever.
    if (s_active.ha_discovery && !c.ha_discovery) publish_discovery(true);
    client_stop();

    lock();
    s_cfg    = c;
    s_active = c;
    unlock();

    // The old link is gone: forget any outage it was timing. Disabled counts
    // as connected for the MQTT-loss trigger, and a re-enabled link starts a
    // fresh 60 s grace on its first disconnect. A client that cannot even
    // start is an outage from now.
    value_feed_mqtt_state(true);
    err = (c.enabled && c.uri[0]) ? client_start() : ESP_OK;
    if (err != ESP_OK) value_feed_mqtt_state(false);

    ESP_LOGI(TAG, "apply: enabled=%d uri=%s discovery=%d", c.enabled,
             c.uri[0] ? c.uri : "(unset)", c.ha_discovery);
    return err;
}

void mqtt_link_get_config(mqtt_cfg_t *out)
{
    if (!out) return;
    lock();
    *out = s_cfg;   // includes the password; the export endpoint is gated
    unlock();
}

void mqtt_link_get_status(mqtt_link_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->compiled_in = true;
    lock();
    out->enabled = s_cfg.enabled;
    unlock();
    out->connected = s_connected;
}

void mqtt_link_publish_state(void)
{
    lock();
    publish_state_locked();
    unlock();
}

#else  // !CONFIG_LEDDISP_MQTT

esp_err_t mqtt_link_init(void)
{
    return ESP_OK;
}

esp_err_t mqtt_link_apply(const mqtt_cfg_t *cfg)
{
    (void)cfg;
    return ESP_ERR_NOT_SUPPORTED;
}

void mqtt_link_get_config(mqtt_cfg_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

void mqtt_link_get_status(mqtt_link_status_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->compiled_in = false;
}

void mqtt_link_publish_state(void)
{
}

#endif  // CONFIG_LEDDISP_MQTT
