// Ambient-light sensor (see light_sensor.h): one sample per second from
// whichever source is configured, exponentially smoothed, mapped through
// light_curve.c and pushed at the scheduler's brightness arbiter whenever the
// level moves materially.
//
// Two sources, one pipeline. The analog path reads an ADC1 pin through
// esp_adc oneshot; the BH1750 path reads lux over I2C and scales it onto the
// same 0..LIGHT_RAW_MAX range. Everything after the read is shared, which is
// what keeps the calibration flow identical for both.
//
// Blob shape in the "cfg" NVS namespace (versioned like every other blob,
// DESIGN.md section 4):
//   "lsen" {"v":2,"enabled":false,"type":"analog","gpio":4,"sda":8,"scl":9,
//           "dark_raw":200,"bright_raw":2800,"min_bri":8,"max_bri":128,
//           "gamma":10}
// A v1 blob has no "type", "sda" or "scl"; it is analog by definition and
// loads unchanged.

#include "light_sensor.h"

#include <string.h>

#include "cJSON.h"
#include "driver/i2c_master.h"
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

// BH1750, GY-302 module. AD0 to GND selects 0x23; the module carries that
// jumper, the board does not.
#define BH1750_ADDR         0x23
#define BH1750_CONT_HRES    0x10   // continuous high resolution, 1 lx, ~120 ms
#define BH1750_I2C_HZ       100000
#define BH1750_TIMEOUT_MS   100

static SemaphoreHandle_t s_lock;
static light_sensor_cfg_t s_cfg;
static adc_oneshot_unit_handle_t s_unit;
static adc_channel_t s_chan;
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static int s_raw   = -1;   // smoothed reading, -1 = none yet
static int s_level = -1;   // level handed to the arbiter, -1 = none yet
static bool s_task_running;
// A source that was armed but has stopped answering. Latched so the log gets
// one line per outage rather than one per second.
static bool s_read_failed;

static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

// ---- config blob ----------------------------------------------------------

static void cfg_defaults(light_sensor_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->enabled = false;
    c->type    = LIGHT_SRC_ANALOG;   // what a device with no stored blob had
    c->gpio    = 0;
    c->sda     = LIGHT_I2C_SDA_DEFAULT;
    c->scl     = LIGHT_I2C_SCL_DEFAULT;
    light_curve_defaults(&c->curve);
}

// Stored as a name so the blob stays readable and an unknown value can fall
// back rather than index off the end of an enum.
static const char *type_name(light_source_t t)
{
    return t == LIGHT_SRC_BH1750 ? "bh1750" : "analog";
}

static bool type_from_name(const char *s, light_source_t *out)
{
    if (!s) return false;
    if (strcmp(s, "analog") == 0) { *out = LIGHT_SRC_ANALOG; return true; }
    if (strcmp(s, "bh1750") == 0) { *out = LIGHT_SRC_BH1750; return true; }
    return false;
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

    // Absent "type" means a v1 blob, which could only ever have been analog.
    // An unrecognised name is treated the same way rather than rejected: the
    // pins alongside it are still meaningful.
    const cJSON *ty = cJSON_GetObjectItem(root, "type");
    if (cJSON_IsString(ty) && !type_from_name(ty->valuestring, &out->type)) {
        ESP_LOGW(TAG, "unknown sensor type '%s' in stored config, using analog",
                 ty->valuestring);
        out->type = LIGHT_SRC_ANALOG;
    }

    out->gpio = (uint8_t)clampi(json_int(root, "gpio", 0), 0, 48);
    out->sda  = (uint8_t)clampi(json_int(root, "sda", out->sda), 0, 48);
    out->scl  = (uint8_t)clampi(json_int(root, "scl", out->scl), 0, 48);
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
    bool pins_ok = (out->type == LIGHT_SRC_BH1750)
        ? (out->sda != out->scl &&
           light_curve_valid_i2c_gpio(out->sda) &&
           light_curve_valid_i2c_gpio(out->scl))
        : (out->gpio == 0 || light_curve_valid_gpio(out->gpio));
    if (!light_curve_valid(&out->curve) || !pins_ok) {
        ESP_LOGW(TAG, "stored light-sensor config rejected, sensor disabled");
        cfg_defaults(out);
    }
}

static esp_err_t save_cfg(const light_sensor_cfg_t *in)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    cJSON_AddNumberToObject(root, "v", 2);
    cJSON_AddBoolToObject(root, "enabled", in->enabled);
    cJSON_AddStringToObject(root, "type", type_name(in->type));
    // Both pin sets are always written, whichever type is active, so switching
    // source and back does not lose the other one's pins.
    cJSON_AddNumberToObject(root, "gpio", in->gpio);
    cJSON_AddNumberToObject(root, "sda", in->sda);
    cJSON_AddNumberToObject(root, "scl", in->scl);
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

// ---- BH1750 ---------------------------------------------------------------

static void bh1750_stop(void)
{
    if (s_dev) {
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
    }
    if (s_bus) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

// Bring up the bus and put the meter into continuous high-resolution mode.
// Internal pull-ups are enabled because the GY-302 carries its own 4k7s: on a
// board that has them the internal ones are harmless, and on a bare module
// without them they are the difference between working and not.
static bool bh1750_start(uint8_t sda, uint8_t scl)
{
    i2c_master_bus_config_t bcfg = {
        .i2c_port                     = I2C_NUM_0,
        .sda_io_num                   = (int)sda,
        .scl_io_num                   = (int)scl,
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bcfg, &s_bus) != ESP_OK) {
        s_bus = NULL;
        ESP_LOGE(TAG, "I2C bus on SDA %u / SCL %u could not be created",
                 (unsigned)sda, (unsigned)scl);
        return false;
    }

    i2c_device_config_t dcfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BH1750_ADDR,
        .scl_speed_hz    = BH1750_I2C_HZ,
    };
    if (i2c_master_bus_add_device(s_bus, &dcfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        bh1750_stop();
        ESP_LOGE(TAG, "could not add the BH1750 at 0x%02x", BH1750_ADDR);
        return false;
    }

    // A missing module fails here, which is the earliest we can tell. The bus
    // stays armed so a sensor plugged in later starts working on its own.
    const uint8_t cmd = BH1750_CONT_HRES;
    if (i2c_master_transmit(s_dev, &cmd, 1, BH1750_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "no BH1750 answering at 0x%02x on SDA %u / SCL %u",
                 BH1750_ADDR, (unsigned)sda, (unsigned)scl);
        return false;
    }
    return true;
}

// One measurement, as lux on the curve's 0..LIGHT_RAW_MAX scale. The meter
// returns counts at 1.2 per lux; the division is done in integers because
// nothing downstream has the resolution to care.
static bool bh1750_read(int *out)
{
    uint8_t b[2];
    if (!s_dev) return false;
    if (i2c_master_receive(s_dev, b, sizeof(b), BH1750_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    int lux = (((int)b[0] << 8 | b[1]) * 10) / 12;
    *out = lux > LIGHT_RAW_MAX ? LIGHT_RAW_MAX : lux;
    return true;
}

// ---- source dispatch ------------------------------------------------------
//
// The only two functions in this file that know which sensor is fitted.

static void source_stop(void)
{
    adc_stop();
    bh1750_stop();
}

// Arm whatever `cfg` asks for. False means "nothing to sample from", which is
// a state the module runs in quite happily: the task keeps ticking and the
// arbiter falls back to the stored brightness.
static bool source_start(const light_sensor_cfg_t *cfg)
{
    s_read_failed = false;
    if (cfg->type == LIGHT_SRC_BH1750) {
        return bh1750_start(cfg->sda, cfg->scl);
    }
    return cfg->gpio != 0 && adc_start(cfg->gpio);
}

static bool source_read(int *out)
{
    if (s_cfg.type == LIGHT_SRC_BH1750) return bh1750_read(out);
    return s_unit && adc_oneshot_read(s_unit, s_chan, out) == ESP_OK;
}

static bool source_armed(void)
{
    return s_cfg.type == LIGHT_SRC_BH1750 ? (s_dev != NULL) : (s_unit != NULL);
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
        if (source_armed() && source_read(&raw)) {
            if (s_read_failed) {
                ESP_LOGI(TAG, "sensor answering again");
                s_read_failed = false;
            }
            s_raw = light_curve_smooth(s_raw, raw);
            uint8_t want = light_curve_brightness(&s_cfg.curve, s_raw);
            if (s_cfg.enabled &&
                light_curve_material(s_level, want, LIGHT_HYSTERESIS)) {
                s_level = want;
                apply = true;
            }
        } else if (source_armed() && !s_read_failed) {
            // Unplugged mid-run, or never answered. One line, then quiet:
            // the task keeps polling so it recovers by itself.
            ESP_LOGW(TAG, "sensor stopped answering; brightness left alone");
            s_read_failed = true;
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
    if (!source_start(&cfg)) {
        ESP_LOGW(TAG, "no usable light sensor; sampling disabled");
    }
    unlock();

    if (!s_task_running) {
        if (xTaskCreate(sensor_task, "lightsen", TASK_STACK, NULL, TASK_PRIO,
                        NULL) != pdPASS) {
            lock();
            source_stop();
            unlock();
            return ESP_ERR_NO_MEM;
        }
        s_task_running = true;
    }

    if (cfg.type == LIGHT_SRC_BH1750) {
        ESP_LOGI(TAG, "init: enabled=%d bh1750 sda=%u scl=%u dark=%u bright=%u "
                      "bri=%u..%u gamma=%u.%u",
                 cfg.enabled, (unsigned)cfg.sda, (unsigned)cfg.scl,
                 (unsigned)cfg.curve.dark_raw, (unsigned)cfg.curve.bright_raw,
                 (unsigned)cfg.curve.min_bri, (unsigned)cfg.curve.max_bri,
                 (unsigned)(cfg.curve.gamma_x10 / 10),
                 (unsigned)(cfg.curve.gamma_x10 % 10));
    } else {
        ESP_LOGI(TAG, "init: enabled=%d analog gpio=%u dark=%u bright=%u "
                      "bri=%u..%u gamma=%u.%u",
                 cfg.enabled, (unsigned)cfg.gpio, (unsigned)cfg.curve.dark_raw,
                 (unsigned)cfg.curve.bright_raw, (unsigned)cfg.curve.min_bri,
                 (unsigned)cfg.curve.max_bri, (unsigned)(cfg.curve.gamma_x10 / 10),
                 (unsigned)(cfg.curve.gamma_x10 % 10));
    }
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
    if (in->type != LIGHT_SRC_ANALOG && in->type != LIGHT_SRC_BH1750) {
        return ESP_ERR_INVALID_ARG;
    }

    if (in->type == LIGHT_SRC_BH1750) {
        // Both pins must be real, usable and distinct. A bus with SDA and SCL
        // on one pin is not a bus.
        if (!light_curve_valid_i2c_gpio(in->sda) ||
            !light_curve_valid_i2c_gpio(in->scl) ||
            in->sda == in->scl) {
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        if (in->gpio != 0 && !light_curve_valid_gpio(in->gpio)) {
            return ESP_ERR_INVALID_ARG;
        }
        // "Enabled" without a pin has nothing to read: refuse rather than
        // store a config that silently does nothing.
        if (in->enabled && in->gpio == 0) return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = save_cfg(in);
    if (err != ESP_OK) return err;

    lock();
    // Any of these means the next sample comes from different hardware, so the
    // smoothed value has to be thrown away with the handles.
    bool source_changed = (s_cfg.type != in->type) ||
                          (s_cfg.gpio != in->gpio) ||
                          (s_cfg.sda  != in->sda)  ||
                          (s_cfg.scl  != in->scl);
    s_cfg = *in;
    if (source_changed) s_raw = -1;
    // Drop the applied level either way: the next sample re-derives it from
    // the new curve, and until then the arbiter falls back to the stored
    // user brightness.
    s_level = -1;
    // Re-arm inside the same critical section: the sampling task must never
    // hold a handle this call has already deleted.
    if (source_changed) {
        source_stop();
        if (!source_start(in)) {
            ESP_LOGW(TAG, "no usable light sensor; sampling disabled");
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
    out->type     = s_cfg.type;
    out->sampling = source_armed() && !s_read_failed;
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
