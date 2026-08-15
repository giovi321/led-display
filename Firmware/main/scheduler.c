#include "scheduler.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "display.h"
#include "integrations.h"
#include "led_driver.h"
#include "light_sensor.h"
#include "settings.h"
#include "tz.h"
#include "webhooks.h"
#include "wifi_manager.h"

static const char *TAG = "sched";
static const char *NS  = "sched";

#define K_ENABLED    "en"
#define K_ON_HOUR    "on_h"
#define K_ON_MIN     "on_m"
#define K_OFF_HOUR   "off_h"
#define K_OFF_MIN    "off_m"
#define K_DAYS       "days"
#define K_UTC_OFF    "utc"
#define K_OFF_ACT    "oact"
#define K_OFF_BRI    "obri"
#define K_TZ         "tz"

static schedule_t s_sched;
static bool       s_display_off;   // the off-window is holding the brightness
static SemaphoreHandle_t s_lock;
// Serialises the whole arbiter (compute + publish) and guards the two flags it
// reads: any task can re-apply, and target_brightness() blocks on an NVS read
// in between, so an unserialised pass could publish a level derived from a
// flag another task has already changed.
static SemaphoreHandle_t s_apply;

// Value-staleness guard state (DESIGN.md section 5). Written only by the
// scheduler task, read by every caller of the arbiter under s_apply.
static bool s_stale;          // currently past the stale threshold
static bool s_stale_dimmed;   // a stale value is halving the brightness

static void apply_defaults(schedule_t *s)
{
    memset(s, 0, sizeof(*s));
    s->enabled    = false;
    s->on_hour    = 7;
    s->on_min     = 0;
    s->off_hour   = 23;
    s->off_min    = 0;
    s->days       = SCHED_DAYS_ALL;
    s->utc_offset = 0;
    // Absent NVS keys (a device updating from an older build) land here, so
    // the off-window keeps blanking the panel exactly as before.
    s->off_action     = SCHED_OFF_ACT_OFF;
    s->off_brightness = SCHED_OFF_BRI_DEFAULT;
}

static esp_err_t load_from_nvs(schedule_t *out)
{
    apply_defaults(out);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err != ESP_OK) return ESP_OK;

    uint8_t v;
    if (nvs_get_u8(h, K_ENABLED, &v) == ESP_OK) out->enabled = v ? true : false;
    nvs_get_u8(h, K_ON_HOUR, &out->on_hour);
    nvs_get_u8(h, K_ON_MIN,  &out->on_min);
    nvs_get_u8(h, K_OFF_HOUR, &out->off_hour);
    nvs_get_u8(h, K_OFF_MIN,  &out->off_min);
    nvs_get_u8(h, K_DAYS,     &out->days);

    int16_t utc;
    if (nvs_get_i16(h, K_UTC_OFF, &utc) == ESP_OK) out->utc_offset = utc;

    if (nvs_get_u8(h, K_OFF_ACT, &v) == ESP_OK && v <= SCHED_OFF_ACT_DIM) {
        out->off_action = v;
    }
    if (nvs_get_u8(h, K_OFF_BRI, &v) == ESP_OK && v > 0) out->off_brightness = v;

    size_t len = sizeof(out->tz);
    if (nvs_get_str(h, K_TZ, out->tz, &len) != ESP_OK) out->tz[0] = '\0';

    nvs_close(h);
    return ESP_OK;
}

// Hand the override to libc so localtime_r() applies it, DST rules included.
// The stored value is an IANA zone name ("Europe/Zurich"), which tz_resolve()
// turns into the POSIX rule newlib actually understands. With no override the
// TZ variable is cleared: localtime then equals gmtime and the fixed
// utc_offset does the shifting, as it always has.
static void apply_tz(const schedule_t *sc)
{
    const char *posix = tz_resolve(sc->tz);
    if (posix) {
        setenv("TZ", posix, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
}

static esp_err_t save_to_nvs(const schedule_t *in)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    // First failure wins and is reported: a full or worn partition fails the
    // sets, not the commit, so discarding these would swallow the only error
    // the caller could act on.
    const struct { const char *key; uint8_t val; } u8s[] = {
        {K_ENABLED,  in->enabled ? 1 : 0},
        {K_ON_HOUR,  in->on_hour},
        {K_ON_MIN,   in->on_min},
        {K_OFF_HOUR, in->off_hour},
        {K_OFF_MIN,  in->off_min},
        {K_DAYS,     in->days},
        {K_OFF_ACT,  in->off_action},
        {K_OFF_BRI,  in->off_brightness},
    };
    for (unsigned i = 0; i < sizeof(u8s) / sizeof(u8s[0]) && err == ESP_OK; i++) {
        err = nvs_set_u8(h, u8s[i].key, u8s[i].val);
    }
    if (err == ESP_OK) err = nvs_set_i16(h, K_UTC_OFF, in->utc_offset);
    if (err == ESP_OK) err = nvs_set_str(h, K_TZ, in->tz);

    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void sntp_start(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    ESP_LOGI(TAG, "SNTP started, server: pool.ntp.org");
}

static bool time_synced(void)
{
    time_t now;
    time(&now);
    return now > 1700000000;
}

static int day_bit_from_tm(const struct tm *t)
{
    // struct tm: tm_wday 0=Sun, 1=Mon ... 6=Sat
    // our bitmask: bit0=Mon ... bit6=Sun
    if (t->tm_wday == 0) return SCHED_DAYS_SUN;
    return 1 << (t->tm_wday - 1);
}

static bool in_on_window(const schedule_t *sc, int hour, int min, int day_bit)
{
    if (!(sc->days & day_bit)) return false;

    int on  = sc->on_hour  * 60 + sc->on_min;
    int off = sc->off_hour * 60 + sc->off_min;
    int now = hour * 60 + min;

    if (on <= off) {
        return now >= on && now < off;
    }
    // Wraps midnight: e.g. on=22:00 off=06:00
    return now >= on || now < off;
}

// Single place that decides how bright the panel runs. The schedule sets the
// level (the in-window level, 0 or off_brightness outside it), that level is
// clamped to the brightness cap, and a stale value halves the result - so the
// staleness dim stays visible in a dim off-window and under a power-budget cap,
// and stays a no-op while the panel is blanked.
//
// The in-window level is the stored user brightness, or the ambient sensor's
// level when the sensor is enabled and has read something. Setup-AP mode is
// the one exception: there the panel carries the only copy of the AP name and
// address, so a dark room must not be able to dim it away - the stored
// brightness wins until the device is back on a network.
static uint8_t target_brightness(void)
{
    schedule_t sc;
    scheduler_get(&sc);

    uint8_t bri;
    if (s_display_off) {
        bri = (sc.off_action == SCHED_OFF_ACT_DIM) ? sc.off_brightness : 0;
    } else {
        settings_t cfg;
        settings_load(&cfg);
        bri = cfg.brightness;

        wifi_mgr_status_t w;
        wifi_manager_get_status(&w);
        uint8_t sensed;
        if (w.mode != WIFI_MGR_MODE_AP && light_sensor_level(&sensed)) {
            bri = sensed;
        }
    }
    // Clamp before the relative dim, so the halving is relative to the level
    // the panel actually shows. Without this a binding cap swallows the stale
    // dim whole: at twice the cap or more, dimmed and undimmed both clamp to
    // the cap and nothing changes.
    uint8_t cap = led_driver_get_brightness_cap();
    if (bri > cap) bri = cap;
    if (s_stale_dimmed) bri /= 2;
    return bri;
}

static void apply_locked(void)   // caller holds s_apply
{
    led_driver_set_brightness(target_brightness());
    led_driver_lock();
    led_driver_refresh();
    led_driver_unlock();
}

static void apply_brightness(void)
{
    xSemaphoreTake(s_apply, portMAX_DELAY);
    apply_locked();
    xSemaphoreGive(s_apply);
}

// Flip one of the arbiter's flags and publish the result in the same critical
// section. Returns whether the flag actually moved, so the callers below only
// log and fire webhooks on a real transition.
static bool set_flag_and_apply(bool *flag, bool value)
{
    xSemaphoreTake(s_apply, portMAX_DELAY);
    bool changed = (*flag != value);
    if (changed) {
        *flag = value;
        apply_locked();
    }
    xSemaphoreGive(s_apply);
    return changed;
}

// The off-window transitions fire their webhooks whichever action is
// configured: consumers care about the window, not the brightness.
static void enter_off_window(void)
{
    if (!set_flag_and_apply(&s_display_off, true)) return;
    ESP_LOGI(TAG, "schedule: off window (brightness=%u)",
             led_driver_get_brightness());
    webhooks_fire(WEBHOOK_EVT_SCHEDULE_OFF, NULL);
}

static void leave_off_window(void)
{
    if (!set_flag_and_apply(&s_display_off, false)) return;
    ESP_LOGI(TAG, "schedule: on window (brightness=%u)",
             led_driver_get_brightness());
    webhooks_fire(WEBHOOK_EVT_SCHEDULE_ON, NULL);
}

// ---- value staleness (DESIGN.md section 5) ---------------------------------
//
// Runs on the same 30 s tick as the schedule. The text action and the webhook
// fire once on the fresh->stale edge; the dim is derived from the live config
// every tick, so switching the action while a value is already stale is picked
// up either way round; the corner marker is re-poked every tick because any
// re-render wipes it (and static text is NOT re-rendered, so between renders
// the poked pixel survives in the frame buffer). Un-staling happens via
// /api/value: the POST re-renders the text (clearing marker/stale text) and
// drops the age, so all this side has to do is put the brightness back.

static void staleness_tick(void)
{
    // Setup-AP mode owns the panel: the banner must stay readable and must not
    // be overwritten by stale text or a marker pixel. The flags reset so the
    // guard re-evaluates (and re-fires its webhook, now deliverable) once the
    // device is back on a network.
    wifi_mgr_status_t w;
    wifi_manager_get_status(&w);
    if (w.mode == WIFI_MGR_MODE_AP) {
        s_stale = false;
        set_flag_and_apply(&s_stale_dimmed, false);
        return;
    }

    // A blanked off-window has nothing to dim, mark or write to, so the guard
    // idles until the on-window returns. A dimmed off-window is a visible
    // display, so it keeps running.
    if (s_display_off) {
        schedule_t sc;
        scheduler_get(&sc);
        if (sc.off_action != SCHED_OFF_ACT_DIM) return;
    }

    // The guard watches the integration value; while the panel shows the
    // static text, a stale value must not dim it, mark it or overwrite it.
    // Same reset-and-idle as the AP branch, so a later switch back to
    // integration mode re-evaluates (and re-fires the webhook if still stale).
    settings_t dcfg;
    settings_load(&dcfg);
    if (dcfg.source != DISPLAY_SRC_INTEGRATION) {
        s_stale = false;
        set_flag_and_apply(&s_stale_dimmed, false);
        return;
    }

    value_cfg_t vc;
    integrations_get_value_cfg(&vc);

    if (vc.stale_hours == 0) {
        // Guard switched off while stale: forget the state, undo the dim.
        set_flag_and_apply(&s_stale_dimmed, false);
        s_stale = false;
        return;
    }

    // age < 0 means no value was ever posted: nothing to go stale.
    int64_t age = integrations_value_age_s();
    bool stale = age >= (int64_t)vc.stale_hours * 3600;

    if (stale && !s_stale) {
        s_stale = true;
        if (vc.stale_action == STALE_ACT_TEXT) {
            display_color_t col;
            display_get_state(NULL, 0, &col, NULL, NULL, NULL);
            display_show_text_static(vc.stale_text, col);
        }
        // none: webhook only. dim: derived below. marker: re-poked below.
        char data[48];
        snprintf(data, sizeof(data), "{\"age_s\":%lld}", (long long)age);
        webhooks_fire(WEBHOOK_EVT_VALUE_STALE, data);
        ESP_LOGW(TAG, "value stale (age=%llds, action=%d)",
                 (long long)age, (int)vc.stale_action);
    } else if (!stale && s_stale) {
        s_stale = false;
        ESP_LOGI(TAG, "value fresh again");
    }

    // Derived from the live config, not latched on the edge: the action can be
    // changed while a value is already stale, in either direction.
    set_flag_and_apply(&s_stale_dimmed,
                       s_stale && vc.stale_action == STALE_ACT_DIM);

    if (s_stale && vc.stale_action == STALE_ACT_MARKER) {
        led_driver_lock();
        led_driver_set_pixel(0, 0, 120, 0, 0);
        led_driver_refresh();
        led_driver_unlock();
    }
}

static void scheduler_task(void *arg)
{
    (void)arg;
    wifi_mgr_mode_t last_mode = WIFI_MGR_MODE_BOOT;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000)); // check every 30 s

        // In setup-AP mode the panel is the only place the AP name and address
        // are readable, so the schedule must not blank it - a device that lost
        // its network at 3 am has to stay findable.
        wifi_mgr_status_t w;
        wifi_manager_get_status(&w);

        // Entering or leaving setup-AP mode moves the arbiter's AP exception in
        // or out of play. main.c re-applies on the state callback; this is the
        // backstop for a mode change that never produced one.
        if (w.mode != last_mode) {
            last_mode = w.mode;
            apply_brightness();
        }

        // Staleness needs no wall-clock time (it works off uptime), so it
        // runs even before SNTP sync.
        if (!time_synced()) {
            staleness_tick();
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        schedule_t sc = s_sched;
        xSemaphoreGive(s_lock);

        if (!sc.enabled || w.mode == WIFI_MGR_MODE_AP) {
            leave_off_window();
        } else {
            time_t now;
            time(&now);
            struct tm t;
            if (sc.tz[0] != '\0') {
                // TZ override: libc applies the rule (set by apply_tz).
                localtime_r(&now, &t);
            } else {
                now += sc.utc_offset * 60;
                gmtime_r(&now, &t);
            }

            int day_bit = day_bit_from_tm(&t);
            bool should_be_on = in_on_window(&sc, t.tm_hour, t.tm_min, day_bit);

            if (should_be_on) {
                leave_off_window();
            } else {
                enter_off_window();
            }
        }

        staleness_tick();
    }
}

esp_err_t scheduler_init(void)
{
    s_lock  = xSemaphoreCreateMutex();
    s_apply = xSemaphoreCreateMutex();
    if (!s_lock || !s_apply) return ESP_ERR_NO_MEM;

    load_from_nvs(&s_sched);
    s_display_off = false;
    apply_tz(&s_sched);

    sntp_start();

    // 4 KB: the staleness path stacks a settings_t + value_cfg_t on top of
    // what the schedule check already used at 3 KB.
    xTaskCreate(scheduler_task, "scheduler", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "scheduler init (enabled=%d on=%02u:%02u off=%02u:%02u days=0x%02x utc=%+d "
                  "off_action=%u off_bri=%u)",
             s_sched.enabled, s_sched.on_hour, s_sched.on_min,
             s_sched.off_hour, s_sched.off_min, s_sched.days,
             s_sched.utc_offset, s_sched.off_action, s_sched.off_brightness);
    return ESP_OK;
}

void scheduler_get(schedule_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_sched;
    xSemaphoreGive(s_lock);
}

esp_err_t scheduler_set(const schedule_t *in)
{
    // NUL-terminate whatever the caller stuffed into the array before the
    // copy is stored, applied or persisted.
    schedule_t sc = *in;
    sc.tz[SCHED_TZ_MAX] = '\0';

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sched = sc;
    xSemaphoreGive(s_lock);
    apply_tz(&sc);
    esp_err_t err = save_to_nvs(&sc);
    // A changed off-window action or level has to show up now, not at the
    // next 30 s tick. Outside the off-window this is a no-op re-apply.
    apply_brightness();
    return err;
}

bool scheduler_display_is_off(void)
{
    return s_display_off;
}

void scheduler_reapply_brightness(void)
{
    // The light sensor can produce a level before scheduler_init() has run
    // (nothing guarantees the boot order for a caller), and the arbiter needs
    // both mutexes.
    if (!s_lock || !s_apply) return;
    apply_brightness();
}
