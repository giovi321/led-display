#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "devname.h"
#include "display.h"
#include "mapping.h"

typedef enum {
    BOOT_SHOW_TEXT = 0,   // borders 1 s -> stored text/scroll state
    BOOT_SHOW_IP   = 1,   // borders 1 s -> scroll the assigned IP / "SETUP"
    BOOT_SHOW_NONE = 2,   // borders 1 s -> blank
} boot_screen_t;

typedef struct {
    char            text[DISPLAY_TEXT_MAX];
    display_color_t color;
    uint8_t         brightness;
    uint8_t         brightness_cap;   // 255 = uncapped (power-budget guard)
    uint8_t         scroll_mode;      // display_scroll_t
    uint8_t         speed;
    uint8_t         scale;
    uint8_t         font;             // FONT_ID_*
    bool            prop;             // proportional glyph spacing
    boot_screen_t   boot_screen;
    // Legacy v1 pixel-mapping mode ("map" NVS key). -1 when absent. Only
    // used once, to migrate to the "layout" blob via mapping_from_legacy().
    int             legacy_map_mode;
} settings_t;

// Populate `out` with sane defaults, then overlay anything we find stored in
// NVS namespace "cfg". Returns ESP_OK whether NVS has data or not.
esp_err_t settings_load(settings_t *out);

// Write the current values to NVS. Cheap to call after each modification.
esp_err_t settings_save(const settings_t *in);

// Updaters that read-modify-write a single field. Useful from HTTP handlers
// so each endpoint doesn't have to know the full schema.
esp_err_t settings_save_text(const char *text,
                             const display_color_t *color,
                             uint8_t scroll_mode,
                             uint8_t speed,
                             uint8_t scale);
esp_err_t settings_save_font(uint8_t font, bool prop);
esp_err_t settings_save_brightness(uint8_t brightness);
esp_err_t settings_save_brightness_cap(uint8_t cap);
esp_err_t settings_save_boot_screen(boot_screen_t which);
esp_err_t settings_clear_text(void);

// ---- device name ----------------------------------------------------------
//
// The single device name (rules in devname.h), stored as the scalar NVS key
// "dname" rather than inside a blob: wifi_manager needs it to set the hostname
// and build the setup-AP SSID before any blob-backed module has initialised.
// `buf` comes back empty when no name is set, which every consumer resolves to
// its own historical default via devname_hostname()/devname_ap_ssid().
esp_err_t settings_load_device_name(char *buf, size_t bufsize);

// An empty or NULL name erases the key (back to the defaults). The caller has
// already validated with devname_valid().
esp_err_t settings_save_device_name(const char *name);

// ---- structured blobs (JSON strings in the same "cfg" namespace) --------
//
// Known keys (see DESIGN.md section 4):
//   "layout"    panel type + grid + cells
//   "colors"    saved colour presets
//   "valuecfg"  value endpoint formatting + staleness
//   "integr"    API key + outbound webhook config
//   "wg"        WireGuard client config
#define SETTINGS_BLOB_MAX  2048

// Read a blob into buf (NUL-terminated). ESP_ERR_NVS_NOT_FOUND when the key
// was never written; ESP_ERR_INVALID_SIZE when it exceeds bufsize.
esp_err_t settings_load_blob(const char *key, char *buf, size_t bufsize,
                             size_t *out_len);
esp_err_t settings_save_blob(const char *key, const char *json);
esp_err_t settings_erase_blob(const char *key);

// Convenience: layout blob <-> mapping_config_t (JSON handled internally,
// shape documented in DESIGN.md section 5 /api/layout).
esp_err_t settings_load_layout(mapping_config_t *out);   // NOT_FOUND if unset
esp_err_t settings_save_layout(const mapping_config_t *in);

// Wipe the whole "cfg" NVS namespace. The WiFi reset already does its own
// namespace; this is for "factory defaults" on the display side.
esp_err_t settings_erase(void);
