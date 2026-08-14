#include "scheduler.h"

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
#include "settings.h"
#include "webhooks.h"

static const char *TAG = "sched";
static const char *NS  = "sched";

#define K_ENABLED    "en"
#define K_ON_HOUR    "on_h"
#define K_ON_MIN     "on_m"
#define K_OFF_HOUR   "off_h"
#define K_OFF_MIN    "off_m"
#define K_DAYS       "days"
#define K_UTC_OFF    "utc"

static schedule_t s_sched;
static bool       s_display_off;
static SemaphoreHandle_t s_lock;

// Value-staleness guard state (DESIGN.md section 5). Only ever touched from
// the scheduler task, so no locking needed.
static bool s_stale;          // currently past the stale threshold
static bool s_stale_dimmed;   // the stale edge halved the brightness

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

    nvs_close(h);
    return ESP_OK;
}

static esp_err_t save_to_nvs(const schedule_t *in)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    nvs_set_u8(h, K_ENABLED,  in->enabled ? 1 : 0);
    nvs_set_u8(h, K_ON_HOUR,  in->on_hour);
    nvs_set_u8(h, K_ON_MIN,   in->on_min);
    nvs_set_u8(h, K_OFF_HOUR, in->off_hour);
    nvs_set_u8(h, K_OFF_MIN,  in->off_min);
    nvs_set_u8(h, K_DAYS,     in->days);
    nvs_set_i16(h, K_UTC_OFF, in->utc_offset);

    err = nvs_commit(h);
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

static void turn_display_off(void)
{
    if (s_display_off) return;
    s_display_off = true;
    led_driver_set_brightness(0);
    led_driver_lock();
    led_driver_refresh();
    led_driver_unlock();
    ESP_LOGI(TAG, "schedule: display OFF");
    webhooks_fire(WEBHOOK_EVT_SCHEDULE_OFF, NULL);
}

static void turn_display_on(void)
{
    if (!s_display_off) return;
    s_display_off = false;
    settings_t cfg;
    settings_load(&cfg);
    // If the value went stale (dim action) while the schedule had us off,
    // come back at the dimmed level, not full brightness.
    uint8_t bri = s_stale_dimmed ? cfg.brightness / 2 : cfg.brightness;
    led_driver_set_brightness(bri);
    led_driver_lock();
    led_driver_refresh();
    led_driver_unlock();
    ESP_LOGI(TAG, "schedule: display ON (brightness=%u)", bri);
    webhooks_fire(WEBHOOK_EVT_SCHEDULE_ON, NULL);
}

// ---- value staleness (DESIGN.md section 5) ---------------------------------
//
// Runs on the same 30 s tick as the schedule. The dim/text actions and the
// webhook fire once on the fresh->stale edge; the corner marker is re-poked
// every tick because any re-render wipes it (and static text is NOT
// re-rendered, so between renders the poked pixel survives in the frame
// buffer). Un-staling happens via /api/value: the POST re-renders the text
// (clearing marker/stale text) and drops the age, so all this side has to
// do is put the brightness back.

static void staleness_tick(void)
{
    if (s_display_off) return;   // schedule turned the display off: don't fight it

    value_cfg_t vc;
    integrations_get_value_cfg(&vc);

    if (vc.stale_hours == 0) {
        // Guard switched off while stale: forget the state, undo the dim.
        if (s_stale_dimmed) {
            settings_t cfg;
            settings_load(&cfg);
            led_driver_set_brightness(cfg.brightness);
            led_driver_lock();
            led_driver_refresh();
            led_driver_unlock();
            s_stale_dimmed = false;
        }
        s_stale = false;
        return;
    }

    // age < 0 means no value was ever posted: nothing to go stale.
    int64_t age = integrations_value_age_s();
    bool stale = age >= (int64_t)vc.stale_hours * 3600;

    if (stale && !s_stale) {
        s_stale = true;
        switch (vc.stale_action) {
        case STALE_ACT_DIM: {
            settings_t cfg;
            settings_load(&cfg);
            led_driver_set_brightness(cfg.brightness / 2);
            led_driver_lock();
            led_driver_refresh();
            led_driver_unlock();
            s_stale_dimmed = true;
            break;
        }
        case STALE_ACT_TEXT: {
            display_color_t col;
            display_get_state(NULL, 0, &col, NULL, NULL, NULL);
            display_show_text_static(vc.stale_text, col);
            break;
        }
        default:
            break;   // none: webhook only; marker: handled below every tick
        }
        char data[48];
        snprintf(data, sizeof(data), "{\"age_s\":%lld}", (long long)age);
        webhooks_fire(WEBHOOK_EVT_VALUE_STALE, data);
        ESP_LOGW(TAG, "value stale (age=%llds, action=%d)",
                 (long long)age, (int)vc.stale_action);
    } else if (!stale && s_stale) {
        s_stale = false;
        if (s_stale_dimmed) {
            settings_t cfg;
            settings_load(&cfg);
            led_driver_set_brightness(cfg.brightness);
            led_driver_lock();
            led_driver_refresh();
            led_driver_unlock();
            s_stale_dimmed = false;
        }
        ESP_LOGI(TAG, "value fresh again");
    }

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
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(30000)); // check every 30 s

        // Staleness needs no wall-clock time (it works off uptime), so it
        // runs even before SNTP sync.
        if (!time_synced()) {
            staleness_tick();
            continue;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        schedule_t sc = s_sched;
        xSemaphoreGive(s_lock);

        if (!sc.enabled) {
            turn_display_on();
        } else {
            time_t now;
            time(&now);
            now += sc.utc_offset * 60;
            struct tm t;
            gmtime_r(&now, &t);

            int day_bit = day_bit_from_tm(&t);
            bool should_be_on = in_on_window(&sc, t.tm_hour, t.tm_min, day_bit);

            if (should_be_on) {
                turn_display_on();
            } else {
                turn_display_off();
            }
        }

        staleness_tick();
    }
}

esp_err_t scheduler_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;

    load_from_nvs(&s_sched);
    s_display_off = false;

    sntp_start();

    // 4 KB: the staleness path stacks a settings_t + value_cfg_t on top of
    // what the schedule check already used at 3 KB.
    xTaskCreate(scheduler_task, "scheduler", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "scheduler init (enabled=%d on=%02u:%02u off=%02u:%02u days=0x%02x utc=%+d)",
             s_sched.enabled, s_sched.on_hour, s_sched.on_min,
             s_sched.off_hour, s_sched.off_min, s_sched.days,
             s_sched.utc_offset);
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
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_sched = *in;
    xSemaphoreGive(s_lock);
    return save_to_nvs(in);
}

bool scheduler_display_is_off(void)
{
    return s_display_off;
}
