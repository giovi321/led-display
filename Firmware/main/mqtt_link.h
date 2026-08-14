#pragma once

// Optional MQTT client (ESP-IDF esp-mqtt). Compiled only when
// CONFIG_LEDDISP_MQTT=y; mqtt_link.c provides no-op stubs otherwise so
// callers never need #ifdefs. Runtime-disabled until the user configures and
// enables it in the Integrations section.
//
// The device is both an integration source ({base}/value/set and
// {base}/text/set feed the panel like POST /api/value and
// /api/display/text) and a controllable/observable node: brightness, power,
// display source and reboot commands, a retained JSON state topic, an
// availability topic (LWT), and optional Home Assistant discovery.
//
// Note: broker authentication is the security boundary here - the API key
// gates HTTP only. Point the device at a broker you trust.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define MQTT_URI_MAX     128  // mqtt://host:1883, mqtts://..., ws(s)://...
#define MQTT_USER_MAX    64
#define MQTT_PASS_MAX    64
#define MQTT_TOPIC_MAX   64   // base topic; empty = the device name
#define MQTT_PREFIX_MAX  32   // HA discovery prefix; empty = "homeassistant"

typedef struct {
    bool enabled;
    char uri[MQTT_URI_MAX + 1];
    char username[MQTT_USER_MAX + 1];        // empty = anonymous
    char password[MQTT_PASS_MAX + 1];
    char base_topic[MQTT_TOPIC_MAX + 1];     // empty = device name
    bool ha_discovery;
    char ha_prefix[MQTT_PREFIX_MAX + 1];     // empty = "homeassistant"
} mqtt_cfg_t;

typedef struct {
    bool compiled_in;   // false when the Kconfig option is off
    bool enabled;
    bool connected;
} mqtt_link_status_t;

// Load config from NVS; if enabled, connect in the background (needs WiFi).
esp_err_t mqtt_link_init(void);

// Save config and apply immediately (tear down / reconnect as needed).
esp_err_t mqtt_link_apply(const mqtt_cfg_t *cfg);

void mqtt_link_get_config(mqtt_cfg_t *out);
void mqtt_link_get_status(mqtt_link_status_t *out);

// Publish the state JSON now (no-op while disconnected). Called after
// externally-visible changes so HA does not wait for the 30 s tick.
void mqtt_link_publish_state(void);
