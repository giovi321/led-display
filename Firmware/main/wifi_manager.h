#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

// How many networks the device remembers. Storage is one NVS key pair per
// slot ("ssid0".."ssid4" / "pass0".."pass4"); slot 0 is mirrored onto the
// pre-2.2 "ssid"/"pass" keys so an OTA rollback still finds credentials.
#define WIFI_MGR_MAX_NETS  5

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
    // SSID of the setup AP, reported in every mode so callers (LED banner,
    // web UI) never have to know the constant themselves.
    char     ap_ssid[33];
} wifi_mgr_status_t;

// One saved network as the API exposes it. Passwords never leave the module.
typedef struct {
    char ssid[33];
    bool connected;   // the STA is on this network right now
} wifi_mgr_saved_t;

// Fires on runtime transitions only (setup AP came up, STA (re)connected), so
// the caller can update the panel. Runs on the reconnect task.
typedef void (*wifi_mgr_state_cb_t)(const wifi_mgr_status_t *st);

esp_err_t wifi_manager_start(void);

void wifi_manager_set_state_cb(wifi_mgr_state_cb_t cb);

// Adopt a new device name (devname.h rules; "" = unset -> defaults). The
// hostname is applied to both netifs immediately - a DHCP server only learns it
// on the next renew or reboot - and the setup-AP SSID changes the next time the
// AP is raised, so a user currently on the portal is never disconnected.
void wifi_manager_set_device_name(const char *name);

// Returns the current mode + connection info. Lightweight; safe to call from
// HTTP handlers.
void wifi_manager_get_status(wifi_mgr_status_t *out);

// Trigger a blocking scan. `out_records` is filled up to `*count` entries;
// `*count` is updated to the number returned. ESP_ERR_TIMEOUT when the
// reconnect logic is holding the radio.
esp_err_t wifi_manager_scan(wifi_ap_record_t *out_records, uint16_t *count);

// ---- saved networks ------------------------------------------------------

// Copy the saved list (SSIDs only) into `out`; returns how many were written.
int wifi_manager_saved_list(wifi_mgr_saved_t *out, int max);

// Add a network, or replace the password of one already saved. Does not switch
// networks itself, but wakes the reconnect pass, so a device that is offline (or
// sitting in the setup AP) tries the new credentials within moments.
// ESP_ERR_NO_MEM when all WIFI_MGR_MAX_NETS slots are taken.
esp_err_t wifi_manager_saved_add(const char *ssid, const char *password);

// Forget one saved network. Leaves an active connection to it alone - the
// device keeps running until it next has to reconnect.
// ESP_ERR_NOT_FOUND when the SSID was not saved.
esp_err_t wifi_manager_saved_delete(const char *ssid);

// Save credentials to NVS (add or update, keeping the other saved networks)
// and schedule a reboot so they take effect from a clean boot.
esp_err_t wifi_manager_save_and_reboot(const char *ssid, const char *password);

// Wipe every saved network and reboot into AP mode.
esp_err_t wifi_manager_reset_and_reboot(void);
