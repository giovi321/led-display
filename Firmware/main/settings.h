#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "devname.h"
#include "display.h"
#include "mapping.h"

// What the panel shows once the network state is known ("after boot").
typedef enum {
    BOOT_SHOW_TEXT    = 0,   // the configured content (static text or integration)
    BOOT_SHOW_IP      = 1,   // scroll the assigned IP
    BOOT_SHOW_NONE    = 2,   // blank
    BOOT_SHOW_IP_THEN = 3,   // the IP for a few seconds, then the content
} boot_screen_t;

// How long BOOT_SHOW_IP_THEN leaves the address up before the content.
#define BOOT_IP_SECONDS 5

// The startup splash, before the network is up. The border is a deliberate
// default: it lights every edge pixel, so a panel that is miswired or half
// dead says so within a second of power-on, before anything else can mask it.
typedef enum {
    STARTUP_BORDER = 0,   // 1 s border test (default)
    STARTUP_BLANK  = 1,   // stay dark until the after-boot screen
    STARTUP_TEXT   = 2,   // go straight to the content, no splash
} startup_screen_t;

// What to show when the WiFi link is down but the device is not running its
// own setup network. Setup-AP mode is deliberately not covered: there the
// panel carries the only copy of the AP name and address, so the banner
// always wins.
typedef enum {
    OFFLINE_KEEP  = 0,    // leave whatever is on the panel (default)
    OFFLINE_TEXT  = 1,    // show offline_text
    OFFLINE_BLANK = 2,    // blank the panel
    OFFLINE_DOT   = 3,    // a single pixel, in offline_color, in a corner
} offline_screen_t;

#define OFFLINE_TEXT_MAX 63

// Where the panel content comes from. Static: the text the user typed in the
// Display tab. Integration: the last value pushed via /api/value (or MQTT),
// with the value config's default_text as the placeholder until one arrives.
typedef enum {
    DISPLAY_SRC_STATIC      = 0,
    DISPLAY_SRC_INTEGRATION = 1,
} display_source_t;

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
    display_source_t source;          // static text vs integration value
    startup_screen_t startup_screen;
    offline_screen_t offline_screen;
    char            offline_text[OFFLINE_TEXT_MAX + 1];
    display_color_t offline_color;    // the OFFLINE_DOT pixel
    uint8_t         offline_corner;   // 0 TL, 1 TR, 2 BL, 3 BR
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
esp_err_t settings_save_source(display_source_t source);
esp_err_t settings_save_screens(const settings_t *in);
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
