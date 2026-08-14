#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "display.h"
#include "integrations.h"
#include "led_driver.h"
#include "mapping.h"
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
    char banner[24];
    if (s->mode == WIFI_MGR_MODE_AP) {
        snprintf(banner, sizeof(banner), "SETUP");
    } else {
        snprintf(banner, sizeof(banner), "%s", s->ip);
    }
    display_show_text(banner, cfg->color, true, 80, cfg->scale);
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

    // 4. Boot splash: 1 s border so we can confirm the LEDs are alive
    //    before anything fancier renders.
    display_draw_border(cfg.color);
    vTaskDelay(pdMS_TO_TICKS(1000));

    // 5. WiFi (STA with timeout, falling back to the captive-portal AP).
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

    // 7. What appears after the splash is user-selectable.
    switch (cfg.boot_screen) {
        case BOOT_SHOW_NONE:
            display_clear();
            break;
        case BOOT_SHOW_IP:
            show_network_banner(&cfg, &s);
            break;
        case BOOT_SHOW_TEXT:
        default:
            // First boot has no saved text: fall back to the IP so the user
            // can find the web UI immediately.
            if (cfg.text[0] == '\0') {
                show_network_banner(&cfg, &s);
            } else {
                display_show_text(cfg.text, cfg.color, cfg.scroll,
                                  cfg.speed, cfg.scale);
            }
            break;
    }

    // 8. Scheduler (SNTP + on/off + value staleness ticks). Needs WiFi up.
    ESP_ERROR_CHECK(scheduler_init());

    // 9. WireGuard (best effort by design: runtime-disabled until
    //    configured, no-op stub when compiled out - never blocks boot).
    if (wg_client_init() != ESP_OK) {
        ESP_LOGW(TAG, "wg_client_init failed; tunnel unavailable");
    }

    // 10. HTTP last so endpoints only become reachable once the display
    //     state is settled.
    ESP_ERROR_CHECK(web_server_start());

    // 11. The web server answering is our "provably up" bar: cancel OTA
    //     rollback so this image sticks. A crash before this point reverts
    //     to the previous slot on the next boot.
    ota_mark_valid();

    // 12. Announce the boot to whatever is listening.
    webhooks_fire(WEBHOOK_EVT_BOOT, NULL);

    ESP_LOGI(TAG, "boot complete (mode=%d, ip=%s, boot_screen=%u, %dx%d)",
             s.mode, s.ip, (unsigned)cfg.boot_screen,
             led_driver_width(), led_driver_height());
}
