// Ambient-light sensor (see light_sensor.h): esp_adc oneshot reads on a 1 Hz
// task, exponentially smoothed, mapped through light_curve.c and pushed at the
// scheduler's brightness arbiter whenever the level moves materially.
//
// Blob shape in the "cfg" NVS namespace (versioned like every other blob,
// DESIGN.md section 4):
//   "lsen" {"v":1,"enabled":false,"gpio":4,"dark_raw":200,"bright_raw":2800,
//           "min_bri":8,"max_bri":128,"gamma":10}

#include "light_sensor.h"

#include <string.h>

#include "cJSON.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "scheduler.h"
#include "settings.h"

static const char *TAG = "lightsen";

#define BLOB_KEY     "lsen"
#define SAMPLE_MS    1000
// 4 KB: a sample itself is tiny, but a level change calls into the arbiter,
// which stacks a settings_t (128-byte text buffer) on top of an NVS read.
#define TASK_STACK   4096
#define TASK_PRIO    2

static SemaphoreHandle_t s_lock;
static light_sensor_cfg_t s_cfg;
static adc_oneshot_unit_handle_t s_unit;
static adc_channel_t s_chan;
static int s_raw   = -1;   // smoothed reading, -1 = none yet
static int s_level = -1;   // level handed to the arbiter, -1 = none yet
static bool s_task_running;

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// ---- config blob ----------------------------------------------------------

static void cfg_defaults(light_sensor_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->enabled = false;
    c->gpio    = 0;
    light_curve_defaults(&c->curve);
}

static int json_int(const cJSON *root, const char *key, int fallback)
{
    const cJSON *it = cJSON_GetObjectItem(root, key);
    return cJSON_IsNumber(it) ? it->valueint : fallback;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void load_cfg(light_sensor_cfg_t *out)
{
    cfg_defaults(out);

    char buf[SETTINGS_BLOB_MAX];
    if (settings_load_blob(BLOB_KEY, buf, sizeof(buf), NULL) != ESP_OK) {
        return;   // never written: defaults, sensor off
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "lsen blob unparseable, using defaults");
        return;
    }

    const cJSON *en = cJSON_GetObjectItem(root, "enabled");
    if (cJSON_IsBool(en)) out->enabled = cJSON_IsTrue(en);

    out->gpio = (uint8_t)clampi(json_int(root, "gpio", 0), 0, 48);
    out->curve.dark_raw =
        (uint16_t)clampi(json_int(root, "dark_raw", out->curve.dark_raw), 0, LIGHT_RAW_MAX);
    out->curve.bright_raw =
        (uint16_t)clampi(json_int(root, "bright_raw", out->curve.bright_raw), 0, LIGHT_RAW_MAX);
    out->curve.min_bri =
        (uint8_t)clampi(json_int(root, "min_bri", out->curve.min_bri), 0, 255);
    out->curve.max_bri =
        (uint8_t)clampi(json_int(root, "max_bri", out->curve.max_bri), 0, 255);
    out->curve.gamma_x10 = (uint8_t)clampi(json_int(root, "gamma", out->curve.gamma_x10),
                                           LIGHT_GAMMA_MIN, LIGHT_GAMMA_MAX);
    cJSON_Delete(root);

    // A stored config that cannot be honoured (hand-edited blob, a pin that a
    // later build no longer allows) falls back to "configured but off" rather
    // than driving the panel from a curve nobody can reason about.
    if (!light_curve_valid(&out->curve) ||
        (out->gpio != 0 && !light_curve_valid_gpio(out->gpio))) {
        ESP_LOGW(TAG, "stored light-sensor config rejected, sensor disabled");
        cfg_defaults(out);
    }
}

static esp_err_t save_cfg(const light_sensor_cfg_t *in)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "v", 1);
    cJSON_AddBoolToObject(root, "enabled", in->enabled);
    cJSON_AddNumberToObject(root, "gpio", in->gpio);
    cJSON_AddNumberToObject(root, "dark_raw", in->curve.dark_raw);
    cJSON_AddNumberToObject(root, "bright_raw", in->curve.bright_raw);
    cJSON_AddNumberToObject(root, "min_bri", in->curve.min_bri);
    cJSON_AddNumberToObject(root, "max_bri", in->curve.max_bri);
    cJSON_AddNumberToObject(root, "gamma", in->curve.gamma_x10);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_ERR_NO_MEM;

    esp_err_t err = settings_save_blob(BLOB_KEY, json);
    cJSON_free(json);
    return err;
}

// ---- ADC ------------------------------------------------------------------

static void adc_stop(void)
{
    if (!s_unit) return;
    adc_oneshot_del_unit(s_unit);
    s_unit = NULL;
}

// Claim the pin. ADC2 pins are refused: that block shares hardware with the
// WiFi radio, so readings from it are unusable on a connected device.
static bool adc_start(uint8_t gpio)
{
    adc_unit_t unit;
    adc_channel_t chan;
    if (adc_oneshot_io_to_channel(gpio, &unit, &chan) != ESP_OK || unit != ADC_UNIT_1) {
        ESP_LOGW(TAG, "GPIO%u is not an ADC1 pin", (unsigned)gpio);
        return false;
    }

    adc_oneshot_unit_init_cfg_t ucfg = {.unit_id = ADC_UNIT_1};
    if (adc_oneshot_new_unit(&ucfg, &s_unit) != ESP_OK) {
        s_unit = NULL;
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed");
        return false;
    }
    // 12 dB attenuation: the whole 0..~3.1 V swing a divider can produce.
    adc_oneshot_chan_cfg_t ccfg = {
        .atten    = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_oneshot_config_channel(s_unit, chan, &ccfg) != ESP_OK) {
        adc_stop();
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed");
        return false;
    }
    s_chan = chan;
    return true;
}

// ---- sampling task --------------------------------------------------------
//
// Sampling runs whenever a usable pin is configured, enabled or not: the
// calibration UI needs live readings before the user is ready to hand the
// brightness over. Only the apply step below is gated on `enabled`.

static void sensor_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));

        // One critical section per sample: the read, the smoothing and the
        // level decision all use state that light_sensor_set() may replace, and
        // the ADC handle itself is deleted when the pin changes. A conversion
        // takes tens of microseconds, so holding the mutex costs nothing.
        bool apply = false;
        lock();
        int raw = 0;
        if (s_unit && adc_oneshot_read(s_unit, s_chan, &raw) == ESP_OK) {
            s_raw = light_curve_smooth(s_raw, raw);
            uint8_t want = light_curve_brightness(&s_cfg.curve, s_raw);
            if (s_cfg.enabled &&
                light_curve_material(s_level, want, LIGHT_HYSTERESIS)) {
                s_level = want;
                apply = true;
            }
        }
        unlock();

        // Don't wait for the 30 s scheduler tick, and don't reach past it
        // either: the arbiter still decides what actually lands on the panel.
        if (apply) scheduler_reapply_brightness();
    }
}

// ---- public API -----------------------------------------------------------

esp_err_t light_sensor_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        if (!s_lock) return ESP_ERR_NO_MEM;
    }

    light_sensor_cfg_t cfg;
    load_cfg(&cfg);

    // s_unit/s_chan belong to the mutex as much as the config does: the task
    // reads them while holding it.
    lock();
    s_cfg   = cfg;
    s_raw   = -1;
    s_level = -1;
    if (cfg.gpio != 0 && !adc_start(cfg.gpio)) {
        ESP_LOGW(TAG, "sensor pin unusable; sampling disabled");
    }
    unlock();

    if (!s_task_running) {
        if (xTaskCreate(sensor_task, "lightsen", TASK_STACK, NULL, TASK_PRIO,
                        NULL) != pdPASS) {
            lock();
            adc_stop();
            unlock();
            return ESP_ERR_NO_MEM;
        }
        s_task_running = true;
    }

    ESP_LOGI(TAG, "init: enabled=%d gpio=%u dark=%u bright=%u bri=%u..%u gamma=%u.%u",
             cfg.enabled, (unsigned)cfg.gpio, (unsigned)cfg.curve.dark_raw,
             (unsigned)cfg.curve.bright_raw, (unsigned)cfg.curve.min_bri,
             (unsigned)cfg.curve.max_bri, (unsigned)(cfg.curve.gamma_x10 / 10),
             (unsigned)(cfg.curve.gamma_x10 % 10));
    return ESP_OK;
}

void light_sensor_get(light_sensor_cfg_t *out)
{
    if (!out) return;
    lock();
    *out = s_cfg;
    unlock();
}

esp_err_t light_sensor_set(const light_sensor_cfg_t *in)
{
    if (!in) return ESP_ERR_INVALID_ARG;
    if (!light_curve_valid(&in->curve)) return ESP_ERR_INVALID_ARG;
    if (in->gpio != 0 && !light_curve_valid_gpio(in->gpio)) return ESP_ERR_INVALID_ARG;
    // "Enabled" without a pin has nothing to read: refuse rather than store a
    // config that silently does nothing.
    if (in->enabled && in->gpio == 0) return ESP_ERR_INVALID_ARG;

    esp_err_t err = save_cfg(in);
    if (err != ESP_OK) return err;

    lock();
    bool pin_changed = (s_cfg.gpio != in->gpio);
    s_cfg = *in;
    if (pin_changed) s_raw = -1;   // a different divider reads differently
    // Drop the applied level either way: the next sample re-derives it from
    // the new curve, and until then the arbiter falls back to the stored
    // user brightness.
    s_level = -1;
    // Re-arm inside the same critical section: the sampling task must never
    // hold a handle this call has already deleted.
    if (pin_changed) {
        adc_stop();
        if (in->gpio != 0 && !adc_start(in->gpio)) {
            ESP_LOGW(TAG, "sensor pin unusable; sampling disabled");
        }
    }
    unlock();

    // Switching the sensor off has to give the panel back to the stored
    // brightness now, not at the next tick.
    scheduler_reapply_brightness();
    return ESP_OK;
}

void light_sensor_get_status(light_sensor_status_t *out)
{
    if (!out) return;
    lock();
    out->enabled  = s_cfg.enabled;
    out->sampling = (s_unit != NULL);
    out->raw      = s_raw;
    out->level    = s_level;
    unlock();
}

bool light_sensor_level(uint8_t *out)
{
    bool have = false;
    lock();
    if (s_cfg.enabled && s_level >= 0) {
        if (out) *out = (uint8_t)s_level;
        have = true;
    }
    unlock();
    return have;
}
