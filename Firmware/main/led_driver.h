#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "mapping.h"

#define LED_DATA_GPIO          16

#define LED_DEFAULT_BRIGHTNESS 32

// Initialise the RMT strip driver and the frame buffer for the given
// mapping. Called once at boot with the stored (or default) config.
esp_err_t led_driver_init(const mapping_config_t *cfg);

// Swap the active mapping at runtime: validates, reallocates the frame
// buffer and LUT, reconfigures the strip length, clears the display.
// Returns ESP_ERR_INVALID_ARG if the config fails mapping_validate().
esp_err_t led_driver_reconfigure(const mapping_config_t *cfg);

// Copy of the active mapping config.
void led_driver_get_config(mapping_config_t *out);

// Logical display size under the active mapping.
int led_driver_width(void);
int led_driver_height(void);
int led_driver_pixels(void);

void led_driver_set_brightness(uint8_t brightness);
uint8_t led_driver_get_brightness(void);

// Cap applied on top of set_brightness (power-budget guard, configurable
// from the advanced UI). 255 = no cap. Persisted by the settings layer.
void led_driver_set_brightness_cap(uint8_t cap);
uint8_t led_driver_get_brightness_cap(void);

void led_driver_clear(void);
void led_driver_fill(uint8_t r, uint8_t g, uint8_t b);

void led_driver_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
bool led_driver_get_pixel(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b);

// Bypass the mapping and write to a raw strip index. Used by the calibration
// test patterns so the user can see how the physical chain is wired.
void led_driver_set_raw(int strip_index, uint8_t r, uint8_t g, uint8_t b);

void led_driver_refresh(void);

void led_driver_lock(void);
void led_driver_unlock(void);
