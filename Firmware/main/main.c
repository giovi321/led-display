#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "api_handlers.h"
#include "display.h"
#include "integrations.h"
#include "led_driver.h"
#include "light_sensor.h"
#include "mapping.h"
#include "mqtt_link.h"
#include "ota_update.h"
#include "scheduler.h"
#include "settings.h"
#include "web_server.h"
#include "webhooks.h"
#include "wg_client.h"
#include "wifi_manager.h"

static const char *TAG = "main";

static void boot_init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// Resolve the active panel arrangement, in priority order: the stored v2
// layout blob, else a one-time migration of the legacy v1 "map" key, else
// the factory default. The migration persists the equivalent layout so the
// legacy key never has to be consulted again.
static void resolve_layout(const settings_t *cfg, mapping_config_t *out)
{
    if (settings_load_layout(out) == ESP_OK) {
        return;
    }
    if (cfg->legacy_map_mode >= 0 &&
        mapping_from_legacy(cfg->legacy_map_mode, out) == MAPPING_OK) {
        if (settings_save_layout(out) != ESP_OK) {
            // Not fatal: the migration just re-runs on the next boot.
            ESP_LOGW(TAG, "failed to persist migrated layout");
        }
        ESP_LOGI(TAG, "migrated legacy map mode %d to layout blob",
                 cfg->legacy_map_mode);
        return;
    }
    mapping_default(out);
}

static void show_network_banner(const settings_t *cfg, const wifi_mgr_status_t *s)
{
    char banner[DISPLAY_TEXT_MAX];
    if (s->mode == WIFI_MGR_MODE_AP) {
        // A device nobody can reach must still say how to reach it, so the AP
        // name and address go on the panel verbatim, and keep going round
        // until somebody sets a text from the web UI. The SSID comes from the
        // WiFi status, never from a local copy of the constant.
        snprintf(banner, sizeof(banner), "AP mode, SSID: %s, IP: %s",
                 s->ap_ssid[0] ? s->ap_ssid : s->ssid, s->ip);
    } else {
        snprintf(banner, sizeof(banner), "%s", s->ip);
    }
    // Continuous so the address repeats after a short gap instead of
    // leaving the panel blank while the user is trying to read it.
    display_show_text(banner, cfg->color, DISPLAY_SCROLL_CONTINUOUS, 80,
                      cfg->scale);
}

// BOOT_SHOW_IP_THEN hands the panel over from the address to the content
// after a few seconds. A task rather than a delay in the boot path, because
// the boot path still has the HTTP server to start; and a generation counter
// so a user who sets something in the meantime is not overwritten by a
// handover they can no longer see coming.
static volatile uint32_t s_handover_gen;

static void ip_handover_task(void *arg)
{
    uint32_t mine = (uint32_t)(uintptr_t)arg;
    vTaskDelay(pdMS_TO_TICKS(BOOT_IP_SECONDS * 1000));
    if (s_handover_gen == mine) {
        api_display_apply_source();
    }
    vTaskDelete(NULL);
}

static void start_ip_handover(void)
{
    uint32_t gen = ++s_handover_gen;
    if (xTaskCreate(ip_handover_task, "boot_ip", 4096,
                    (void *)(uintptr_t)gen, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no room for the boot handover task; showing content now");
        api_display_apply_source();
    }
}

// What the panel shows once the network state is known. AP mode overrides the
// boot-screen choice: "blank" or a stored text on an unreachable device leaves
// the user with nothing to type into a browser.
static void show_boot_screen(const settings_t *cfg, const wifi_mgr_status_t *s)
{
    if (s->mode == WIFI_MGR_MODE_AP) {
        show_network_banner(cfg, s);
        return;
    }
    switch (cfg->boot_screen) {
        case BOOT_SHOW_NONE:
            display_clear();
            break;
        case BOOT_SHOW_IP:
            show_network_banner(cfg, s);
            break;
        case BOOT_SHOW_IP_THEN:
            // The address first so it can be read off the panel, then the
            // content. The handover is a short task rather than a delay here:
            // this runs during boot, and the HTTP server must not wait on it.
            show_network_banner(cfg, s);
            start_ip_handover();
            break;
        case BOOT_SHOW_TEXT:
        default:
            if (cfg->source == DISPLAY_SRC_INTEGRATION) {
                // The last value never survives a reboot; the configured
                // placeholder stands in until the next push. No placeholder
                // set: blank, so a stale-looking static text never poses as
                // live data.
                value_cfg_t vc;
                integrations_get_value_cfg(&vc);
                if (vc.default_text[0] != '\0') {
                    display_show_text(vc.default_text, cfg->color,
                                      cfg->scroll_mode, cfg->speed, cfg->scale);
                } else {
                    display_clear();
                }
                break;
            }
            // First boot has no saved text: fall back to the IP so the user
            // can find the web UI immediately.
            if (cfg->text[0] == '\0') {
                show_network_banner(cfg, s);
            } else {
                display_show_text(cfg->text, cfg->color, cfg->scroll_mode,
                                  cfg->speed, cfg->scale);
            }
            break;
    }
}

// Runtime network transitions (setup AP raised after a long outage, STA link
// restored). Only the AP banner and its removal are handled here: whatever
// else is on the panel - a pushed value, a test pattern - stays untouched.
static bool s_ap_banner;

// Whether the offline screen is currently on the panel, so the configured
// content only has to be restored once, on the transition back.
static bool s_offline_screen;

static void on_wifi_state(const wifi_mgr_status_t *s)
{
    settings_t cfg;
    settings_load(&cfg);

    if (s->mode == WIFI_MGR_MODE_AP) {
        s_ap_banner = true;
        s_offline_screen = false;
        show_network_banner(&cfg, s);
    } else if (s_ap_banner && s->connected) {
        s_ap_banner = false;
        show_boot_screen(&cfg, s);
    } else if (!s->connected && cfg.offline_screen != OFFLINE_KEEP) {
        // Link lost while still in station mode. AP mode is handled above and
        // deliberately overrides this: there the panel is the only place the
        // setup address appears.
        if (!s_offline_screen) {
            s_offline_screen = true;
            if (cfg.offline_screen == OFFLINE_TEXT && cfg.offline_text[0]) {
                display_show_text(cfg.offline_text, cfg.color, cfg.scroll_mode,
                                  cfg.speed, cfg.scale);
            } else if (cfg.offline_screen == OFFLINE_DOT) {
                display_show_dot(cfg.offline_corner, cfg.offline_color);
            } else {
                display_clear();
            }
        }
    } else if (s->connected && s_offline_screen) {
        s_offline_screen = false;
        show_boot_screen(&cfg, s);
    }

    // The mode change moves the arbiter's setup-AP exception in or out of play
    // (AP mode ignores the light sensor so the banner stays readable), and
    // nothing else re-evaluates it: the sensor only pushes on a material level
    // change and the schedule/staleness applies are edge-guarded. Idempotent.
    scheduler_reapply_brightness();
}

void app_main(void)
{
    ESP_LOGI(TAG, "led-display starting up");
    boot_init_nvs();

    // 1. Load saved settings (or sane defaults on first boot).
    settings_t cfg;
    settings_load(&cfg);

    // 2. Resolve the panel arrangement (stored layout / legacy migration /
    //    factory default).
    mapping_config_t layout;
    resolve_layout(&cfg, &layout);

    // 3. Bring the LED stack up and apply persisted hardware state (cap
    //    before brightness, so an over-cap value never reaches the panel)
    //    before any pixel is drawn.
    ESP_ERROR_CHECK(led_driver_init(&layout));
    led_driver_set_brightness_cap(cfg.brightness_cap);
    led_driver_set_brightness(cfg.brightness);
    ESP_ERROR_CHECK(display_init());
    display_set_font(cfg.font, cfg.prop);
    display_set_fade(cfg.fade);

    // 4. Startup splash. The border lights every edge pixel, so a miswired or
    //    half-dead panel says so within a second of power-on; that is why it
    //    is the default. The other choices skip it.
    if (cfg.startup_screen == STARTUP_BORDER) {
        display_draw_border(cfg.color);
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else if (cfg.startup_screen == STARTUP_BLANK) {
        display_clear();
    }

    // 5. WiFi (scan, then the saved networks strongest-first; falls back to
    //    the captive-portal AP when none of them answers).
    ESP_ERROR_CHECK(wifi_manager_start());

    wifi_mgr_status_t s;
    wifi_manager_get_status(&s);

    // 6. Integration config and the outbound webhook worker. Neither may
    //    block boot: a missing blob or full heap just means no events.
    if (integrations_init() != ESP_OK) {
        ESP_LOGW(TAG, "integrations_init failed; continuing with defaults");
    }
    if (webhooks_init() != ESP_OK) {
        ESP_LOGW(TAG, "webhooks_init failed; outbound events disabled");
    }

    // 7. What appears after the splash is user-selectable - except in AP
    //    mode, where the setup banner always wins.
    s_ap_banner = (s.mode == WIFI_MGR_MODE_AP);
    show_boot_screen(&cfg, &s);
    wifi_manager_set_state_cb(on_wifi_state);

    // 8. Scheduler (SNTP + on/off + value staleness ticks). Needs WiFi up.
    ESP_ERROR_CHECK(scheduler_init());

    // 9. Optional ambient-light sensor. After the scheduler on purpose: a
    //    fresh reading re-applies the brightness through the scheduler's
    //    arbiter, which needs its mutex to exist.
    if (light_sensor_init() != ESP_OK) {
        ESP_LOGW(TAG, "light_sensor_init failed; brightness stays manual");
    }

    // 10. HTTP so endpoints become reachable once the display state is
    //     settled.
    ESP_ERROR_CHECK(web_server_start());

    // 11. The web server answering is our "provably up" bar: cancel OTA
    //     rollback so this image sticks. A crash before this point reverts
    //     to the previous slot on the next boot.
    ota_mark_valid();

    // 12. The optional outbound clients start only now, after the image has
    //     been committed. They dial out to networks and peers this firmware
    //     has never met, so they are the most likely thing in the boot to
    //     fault - and before this point a fault would roll back a perfectly
    //     good update, hiding the real problem behind a version that
    //     silently reappears. Both are best effort by design: runtime
    //     disabled until configured, no-op stubs when compiled out, and
    //     neither blocks the boot.
    if (wg_client_init() != ESP_OK) {
        ESP_LOGW(TAG, "wg_client_init failed; tunnel unavailable");
    }
    if (mqtt_link_init() != ESP_OK) {
        ESP_LOGW(TAG, "mqtt_link_init failed; MQTT unavailable");
    }

    // 13. Announce the boot to whatever is listening.
    webhooks_fire(WEBHOOK_EVT_BOOT, NULL);

    ESP_LOGI(TAG, "boot complete (mode=%d, ip=%s, boot_screen=%u, %dx%d)",
             s.mode, s.ip, (unsigned)cfg.boot_screen,
             led_driver_width(), led_driver_height());
}
