#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

typedef enum {
    WIFI_MGR_MODE_BOOT = 0,
    WIFI_MGR_MODE_STA,
    WIFI_MGR_MODE_AP,
} wifi_mgr_mode_t;

typedef struct {
    wifi_mgr_mode_t mode;
    bool     connected;
    char     ssid[33];
    char     ip[16];
    int8_t   rssi;
} wifi_mgr_status_t;

esp_err_t wifi_manager_start(void);

// Returns the current mode + connection info. Lightweight; safe to call from
// HTTP handlers.
void wifi_manager_get_status(wifi_mgr_status_t *out);

// Trigger a blocking scan. `out_records` is filled up to `*count` entries;
// `*count` is updated to the number returned.
esp_err_t wifi_manager_scan(wifi_ap_record_t *out_records, uint16_t *count);

// Save credentials to NVS and schedule a reboot so the new creds take effect
// from a clean boot. Returns ESP_OK if the credentials were persisted.
esp_err_t wifi_manager_save_and_reboot(const char *ssid, const char *password);

// Wipe stored credentials and reboot into AP mode.
esp_err_t wifi_manager_reset_and_reboot(void);
