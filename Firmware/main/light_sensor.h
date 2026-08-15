#pragma once

// Optional ambient-light sensor: a BH1750 digital lux meter (the GY-302
// module) on I2C, whose reading drives the panel brightness. The v4 controller
// board has a connector for it.
//
// Only the raw-sample source is hardware-specific. The curve
// (light_curve.[hc]) and the scheduler's brightness arbiter both take a plain
// 0..LIGHT_RAW_MAX number, so the sensor feeds lux in on that scale and
// nothing downstream knows or cares where a sample came from.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "light_curve.h"

typedef struct {
    bool          enabled;   // drive the panel brightness from the reading
    uint8_t       sda;       // I2C pins the sensor is wired to
    uint8_t       scl;
    light_curve_t curve;
} light_sensor_cfg_t;

typedef struct {
    bool enabled;
    // The sensor is configured AND answering. One that stops replying
    // (unplugged, miswired, ADDR not grounded) drops this to false, so the UI
    // can say so instead of showing a frozen reading.
    bool sampling;
    int  raw;      // smoothed reading in lux, -1 = nothing sampled yet
    int  level;    // brightness the curve currently asks for, -1 = none yet
} light_sensor_status_t;

// Loads the stored config and starts the sampling task. Call after
// scheduler_init(): a fresh reading re-applies the brightness through the
// scheduler's arbiter.
esp_err_t light_sensor_init(void);

void light_sensor_get(light_sensor_cfg_t *out);

// Validate, persist, re-arm the bus and re-apply the brightness. Returns
// ESP_ERR_INVALID_ARG on a curve that fails light_curve_valid(), a pin
// light_curve_valid_i2c_gpio() refuses, or SDA and SCL on the same pin.
esp_err_t light_sensor_set(const light_sensor_cfg_t *in);

void light_sensor_get_status(light_sensor_status_t *out);

// True when the sensor is enabled and has a level to impose; `*out` then holds
// it. scheduler.c uses this in place of the stored user brightness inside the
// on-window - the stored value is never overwritten, so switching the sensor
// off restores it.
bool light_sensor_level(uint8_t *out);
