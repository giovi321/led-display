#pragma once

// OTA firmware updates. Two paths into the same esp_ota machinery:
//   - web upload: api_ota.c streams the request body through
//     ota_begin/ota_write_chunk/ota_end_and_reboot
//   - URL pull: ota_from_url() runs esp_https_ota in a worker task
// Rollback is armed via CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE; main.c calls
// ota_mark_valid() once the web server is up so a broken image reverts on
// the next boot.

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct {
    char running_version[32];
    char running_slot[20];    // "factory", "ota_0", "ota_1" (partition labels are <= 16 chars + NUL)
    char last_result[96];     // human-readable outcome of the last attempt
    bool in_progress;
    int  progress_pct;        // 0..100, -1 = unknown size
} ota_status_t;

esp_err_t ota_begin(size_t image_size_or_zero);
esp_err_t ota_write_chunk(const void *data, size_t len);
// Finalise, set boot partition, schedule a reboot (after the HTTP response
// has gone out). Returns an error and stays on the running image if the
// image fails validation.
esp_err_t ota_end_and_reboot(void);
void ota_abort_update(void);

// Async: download from `url` (http or https with the IDF cert bundle) and
// apply. Progress/result via ota_get_status() and the OTA_RESULT webhook.
esp_err_t ota_from_url(const char *url);

void ota_get_status(ota_status_t *out);

// Query the latest GitHub release of CONFIG_LEDDISP_GITHUB_REPO and compare
// it against the running FW_VERSION. Blocking (one HTTPS round trip, ~10 s
// timeout worst case); called from the /api/ota/check handler.
typedef struct {
    char current[32];
    char latest[32];       // release tag without a leading 'v'; "" on error
    char asset_url[256];   // browser_download_url of led_display.bin
    bool update_available;
    char error[96];        // "" on success
} ota_check_t;

esp_err_t ota_check_github(ota_check_t *out);

// Cancel rollback for the running image (call once the app is provably up).
void ota_mark_valid(void);
