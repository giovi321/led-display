#pragma once

// Ambient-light curve: one raw ADC reading -> one panel brightness. Pure
// module (no ESP-IDF, no state) so the mapping, the smoothing and the
// hysteresis rule are host-tested; light_sensor.c owns the hardware and the
// task around it.

#include <stdbool.h>
#include <stdint.h>

#define LIGHT_RAW_MAX      4095   // 12-bit ADC full scale
#define LIGHT_GAMMA_MIN    5      // 0.5 in tenths
#define LIGHT_GAMMA_MAX    40     // 4.0
#define LIGHT_GAMMA_UNITY  10     // 1.0 = straight line between the two ends

// ESP32-S3: ADC1 channels 0..9 sit on GPIO1..GPIO10. ADC2 shares hardware
// with the WiFi radio and reads garbage (or fails) while connected, so only
// ADC1 pins are offered.
#define LIGHT_GPIO_MIN     1
#define LIGHT_GPIO_MAX     10

// Reference pins for the BH1750 on the v4 controller board's
// "GND SDA SCL 5V" connector. Only defaults: both are configurable.
#define LIGHT_I2C_SDA_DEFAULT  8
#define LIGHT_I2C_SCL_DEFAULT  9

// Brightness steps the level has to move before the panel is re-driven.
// Keeps a reading that sits on a boundary from stepping up and down forever.
#define LIGHT_HYSTERESIS   3

typedef struct {
    uint16_t dark_raw;    // reading with the sensor covered
    uint16_t bright_raw;  // reading in the light the panel should be brightest in
    uint8_t  min_bri;     // brightness at dark_raw
    uint8_t  max_bri;     // brightness at bright_raw
    uint8_t  gamma_x10;   // shape between the ends, tenths (10 = linear)
} light_curve_t;

void light_curve_defaults(light_curve_t *c);

// Range check for a stored/posted curve: the two calibration points must
// differ (a zero-width window has no slope) and gamma must be in range.
bool light_curve_valid(const light_curve_t *c);

bool light_curve_valid_gpio(int gpio);

// Whether `gpio` can carry an I2C line on the ESP32-S3-N16R8 this board is
// built around. Rejects, in order: anything outside 0..48; GPIO22 to GPIO25,
// which the S3 does not bring out at all; the strapping pins 0, 3, 45 and 46,
// which are sampled at reset and would be pulled by an I2C bus; and GPIO26 to
// GPIO37, which are wired to the module's own SPI flash and octal PSRAM.
// Taking one of those last two groups out is not a configuration error you
// recover from - it is the module failing to boot.
bool light_curve_valid_i2c_gpio(int gpio);

// raw (0..LIGHT_RAW_MAX, clamped) -> brightness. dark_raw/bright_raw may come
// in either numeric order (a pulled-up LDR reads high in the dark), so a
// calibration pair captured from live readings always maps the right way round
// and no separate "inverted sensor" switch is needed.
uint8_t light_curve_brightness(const light_curve_t *c, int raw);

// Exponential smoothing, quarter weight on the new sample. `prev` < 0 seeds
// from the first reading. Rounds away from `prev` so the value still converges
// on small differences instead of parking a few counts short.
int light_curve_smooth(int prev, int raw);

// Should `candidate` replace the currently applied level? True on the first
// reading (`applied` < 0) and when the difference reaches `hyst`. Reaching 0 is
// always material - going fully dark is visible even as a single step - but
// leaving 0 needs the full band, or a reading sitting on the 0/1 boundary would
// blink the panel on and off once per sample.
bool light_curve_material(int applied, int candidate, int hyst);
