#pragma once

// Optional ambient-light sensor: an analog sensor (LDR voltage divider, analog
// lux module) on an ADC1 pin whose reading drives the panel brightness.
//
// Only the raw-sample source is hardware-specific here. The curve
// (light_curve.[hc]) and the scheduler's brightness arbiter both take a plain
// 0..LIGHT_RAW_MAX number, so a digital (I2C) lux meter would only have to
// feed this module samples - nothing downstream knows where they came from.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "light_curve.h"

typedef struct {
    bool          enabled;   // drive the panel brightness from the reading
    uint8_t       gpio;      // ADC1 pin (LIGHT_GPIO_MIN..MAX), 0 = none set
    light_curve_t curve;
} light_sensor_cfg_t;

typedef struct {
    bool enabled;
    bool sampling;   // a usable pin is configured and being read
    int  raw;        // smoothed reading, -1 = nothing sampled yet
    int  level;      // brightness the curve currently asks for, -1 = none yet
} light_sensor_status_t;

// Loads the stored config and, when a usable pin is set, starts the sampling
// task. Call after scheduler_init(): a fresh reading re-applies the brightness
// through the scheduler's arbiter.
esp_err_t light_sensor_init(void);

void light_sensor_get(light_sensor_cfg_t *out);

// Validate, persist, re-arm the ADC and re-apply the brightness. Returns
// ESP_ERR_INVALID_ARG on a pin outside ADC1 or a curve that fails
// light_curve_valid().
esp_err_t light_sensor_set(const light_sensor_cfg_t *in);

void light_sensor_get_status(light_sensor_status_t *out);

// True when the sensor is enabled and has a level to impose; `*out` then holds
// it. scheduler.c uses this in place of the stored user brightness inside the
// on-window - the stored value is never overwritten, so switching the sensor
// off restores it.
bool light_sensor_level(uint8_t *out);
