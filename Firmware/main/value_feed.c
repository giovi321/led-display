#include "value_feed.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "display.h"
#include "integrations.h"
#include "led_driver.h"
#include "mqtt_link.h"
#include "scheduler.h"
#include "settings.h"
#include "valuefmt.h"
#include "webhooks.h"
#include "wifi_manager.h"

static const char *TAG = "value_feed";

static SemaphoreHandle_t s_lock;
static fb_state_t        s_fb;
// The fallback dim, derived under s_lock after every state, config or source
// change. Read lock-free by the scheduler's brightness arbiter, which holds
// its own mutex while it reads: taking s_lock there would invert the order
// against value_feed re-applying the brightness.
static volatile bool     s_dim;

static const char *SRC_NAMES[] = {"http", "mqtt", "poll"};

// Monotonic uptime, never wall clock: SNTP steps must not fire triggers.
static int64_t now_s(void)
{
    return esp_timer_get_time() / 1000000;
}

// Tolerate pre-init callers (a value can only arrive after init, but the
// scheduler arbiter and the MQTT stubs must never crash on a NULL mutex).
static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// ---- panel effects (caller holds s_lock) ------------------------------------

// No value since boot shows the fallback text whatever the mode: there is no
// last value to mark or dim.
static bool shows_text_locked(const value_cfg_t *vc)
{
    return s_fb.active && (vc->fb.mode == FB_MODE_TEXT || !s_fb.have_value);
}

static bool keeps_value_locked(const value_cfg_t *vc, fb_mode_t mode)
{
    return s_fb.active && s_fb.have_value && vc->fb.mode == mode;
}

// Recompute the dim flag; returns whether it moved (the caller re-applies the
// brightness after unlocking).
static bool update_dim_locked(const value_cfg_t *vc, const settings_t *cfg)
{
    bool dim = cfg->source == DISPLAY_SRC_INTEGRATION &&
               keeps_value_locked(vc, FB_MODE_DIM);
    bool changed = (dim != s_dim);
    s_dim = dim;
    return changed;
}

// Fallback text, or a blank panel when it is empty. Current colour, scroll
// mode and speed; scale 0 keeps the active scale.
static void show_fallback_text_locked(const value_cfg_t *vc)
{
    if (vc->fb.text[0] == '\0') {
        display_clear();
        return;
    }
    display_color_t col;
    uint8_t mode, speed;
    display_get_state(NULL, 0, &col, &mode, &speed, NULL);
    display_show_text(vc->fb.text, col, mode, speed, 0);
}

// Red pixel at (0,0). Any re-render wipes it, so the tick re-pokes it; static
// text is not re-rendered between ticks, so the poked pixel survives in the
// frame buffer. Skipped on a blank panel (power off, empty text) and in a
// blanked off-window, where there is nothing to mark.
static void poke_marker_locked(void)
{
    char text[DISPLAY_TEXT_MAX];
    display_get_state(text, sizeof(text), NULL, NULL, NULL, NULL);
    if (text[0] == '\0') return;
    if (scheduler_display_is_off()) {
        schedule_t sc;
        scheduler_get(&sc);
        if (sc.off_action != SCHED_OFF_ACT_DIM) return;
    }
    led_driver_lock();
    led_driver_set_pixel(0, 0, 120, 0, 0);
    led_driver_refresh();
    led_driver_unlock();
}

// After the fallback engaged: show what the mode asks for. DIM is handled by
// the caller through update_dim_locked().
static void on_engaged_panel_locked(const value_cfg_t *vc, const settings_t *cfg)
{
    if (cfg->source != DISPLAY_SRC_INTEGRATION) return;
    if (shows_text_locked(vc)) {
        show_fallback_text_locked(vc);
    } else if (keeps_value_locked(vc, FB_MODE_MARKER)) {
        poke_marker_locked();
    }
}

// ---- reporting (never under s_lock) -----------------------------------------

static void report_engaged(fb_reason_t reason)
{
    if (!fb_notifies(reason)) return;
    int64_t age = integrations_value_age_s();
    char data[64];
    snprintf(data, sizeof(data), "{\"reason\":\"%s\",\"age_s\":%lld}",
             fb_reason_str(reason), (long long)age);
    ESP_LOGW(TAG, "fallback engaged (reason=%s, age=%llds)",
             fb_reason_str(reason), (long long)age);
    webhooks_fire(WEBHOOK_EVT_VALUE_STALE, data);
    mqtt_link_publish_state();
}

static void report_recovered(fb_reason_t prev)
{
    if (!fb_notifies(prev)) return;
    char data[48];
    snprintf(data, sizeof(data), "{\"reason\":\"%s\"}", fb_reason_str(prev));
    ESP_LOGI(TAG, "fallback cleared (was %s)", fb_reason_str(prev));
    webhooks_fire(WEBHOOK_EVT_VALUE_RECOVERED, data);
    mqtt_link_publish_state();
}

// ---- public API -------------------------------------------------------------

esp_err_t value_feed_init(void)
{
    // Boot, single-threaded: nothing else touches the state yet. Initialised
    // even when the mutex cannot be created, so the state machine is sane.
    fb_init(&s_fb, now_s());
    s_dim = false;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

void value_feed_ingest(const char *raw, value_src_t src, char *out, size_t outsz)
{
    value_cfg_t vc;
    integrations_get_value_cfg(&vc);
    settings_t cfg;
    settings_load(&cfg);

    char text[DISPLAY_TEXT_MAX];
    valuefmt_format(&vc, raw ? raw : "", text, sizeof(text));
    if (out && outsz) snprintf(out, outsz, "%s", text);

    lock();
    // Always remembered (so switching to integration later shows it) and
    // always counted as fresh, whatever the display source.
    integrations_set_last_value(text);
    integrations_touch_value();
    // Read before fb_on_value: it also reports CLEARED when leaving the boot
    // NO_VALUE state, which must not send value_recovered.
    fb_reason_t prev = s_fb.reason;
    fb_change_t ch   = fb_on_value(&s_fb, now_s());
    if (cfg.source == DISPLAY_SRC_INTEGRATION) {
        // Current colour, scrolling mode and speed; scale 0 keeps the active
        // scale. Not persisted as the stored text: that is the static text.
        // The re-render also wipes a corner marker.
        display_color_t col;
        uint8_t mode, speed;
        display_get_state(NULL, 0, &col, &mode, &speed, NULL);
        display_show_text(text, col, mode, speed, 0);
    }
    bool dim_moved = update_dim_locked(&vc, &cfg);
    unlock();

    ESP_LOGD(TAG, "value from %s",
             (unsigned)src < sizeof(SRC_NAMES) / sizeof(SRC_NAMES[0])
                 ? SRC_NAMES[src] : "?");

    if (dim_moved) scheduler_reapply_brightness();
    if (ch == FB_CHANGE_CLEARED) report_recovered(prev);
}

void value_feed_poll_failed(void)
{
    value_cfg_t vc;
    integrations_get_value_cfg(&vc);
    settings_t cfg;
    settings_load(&cfg);

    lock();
    fb_change_t ch = fb_on_poll_result(&s_fb, &vc.fb, false, now_s());
    fb_reason_t reason = s_fb.reason;
    if (ch == FB_CHANGE_ENGAGED) on_engaged_panel_locked(&vc, &cfg);
    bool dim_moved = update_dim_locked(&vc, &cfg);
    unlock();

    if (dim_moved) scheduler_reapply_brightness();
    if (ch == FB_CHANGE_ENGAGED) report_engaged(reason);
}

void value_feed_mqtt_state(bool connected)
{
    lock();
    // Only records the state; the MQTT-loss trigger fires from the tick.
    (void)fb_on_mqtt(&s_fb, connected, now_s());
    unlock();
}

void value_feed_tick(void)
{
    // Setup-AP mode owns the panel: the banner must stay readable.
    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);
    if (w.mode == WIFI_MGR_MODE_AP) return;

    // Gathered before taking s_lock: none of these may nest inside it.
    mqtt_link_status_t ms;
    mqtt_link_get_status(&ms);
    value_cfg_t vc;
    integrations_get_value_cfg(&vc);
    settings_t cfg;
    settings_load(&cfg);

    lock();
    fb_change_t ch = fb_tick(&s_fb, &vc.fb, ms.enabled, now_s());
    fb_reason_t reason = s_fb.reason;
    if (ch == FB_CHANGE_ENGAGED) {
        on_engaged_panel_locked(&vc, &cfg);
    } else if (cfg.source == DISPLAY_SRC_INTEGRATION &&
               keeps_value_locked(&vc, FB_MODE_MARKER)) {
        poke_marker_locked();
    }
    // Derived every tick: the mode can change while the fallback is active.
    bool dim_moved = update_dim_locked(&vc, &cfg);
    unlock();

    if (dim_moved) scheduler_reapply_brightness();
    if (ch == FB_CHANGE_ENGAGED) report_engaged(reason);
}

void value_feed_get_state(value_feed_state_t *out)
{
    if (!out) return;
    value_cfg_t vc;
    integrations_get_value_cfg(&vc);
    lock();
    out->active = s_fb.active;
    out->reason = s_fb.reason;
    unlock();
    out->mode = vc.fb.mode;
}

bool value_feed_dim_active(void)
{
    // The setup-AP banner is never dimmed. main.c and the scheduler tick
    // re-apply the brightness on every mode change.
    if (!s_dim) return false;
    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);
    return w.mode != WIFI_MGR_MODE_AP;
}

void value_feed_apply_panel(void)
{
    value_cfg_t vc;
    integrations_get_value_cfg(&vc);
    settings_t cfg;
    settings_load(&cfg);

    lock();
    if (cfg.source == DISPLAY_SRC_INTEGRATION) {
        char text[DISPLAY_TEXT_MAX];
        if (shows_text_locked(&vc)) {
            snprintf(text, sizeof(text), "%s", vc.fb.text);
        } else if (!integrations_get_last_value(text, sizeof(text))) {
            text[0] = '\0';
        }
        if (text[0] != '\0') {
            display_show_text(text, cfg.color, cfg.scroll_mode, cfg.speed,
                              cfg.scale);
        } else {
            // Nothing to show; display_clear also ends identify/test modes.
            display_clear();
        }
        if (keeps_value_locked(&vc, FB_MODE_MARKER)) poke_marker_locked();
    }
    update_dim_locked(&vc, &cfg);
    unlock();

    // Always: the source or the mode may have moved the dim, and a caller
    // switching to the static text must drop a dim it was holding.
    scheduler_reapply_brightness();
}
