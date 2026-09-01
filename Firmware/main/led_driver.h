#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "mapping.h"
#include "power_model.h"

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

// ---- power budget ---------------------------------------------------------
//
// The brightness cap is a blunt instrument: it scales RGB without knowing how
// much current the frame will actually draw. The power budget is the calculated
// version. Every refresh sums the frame's channel bytes and runs them through
// power_model, which gives both a live estimate to report and, when the limiter
// is enabled, the brightness that frame has to go out at to stay inside the
// budget.
//
// The limiter is per-frame and presentation-only: the stored brightness is
// never rewritten, so lowering the content's coverage or its colour restores
// the level the user asked for without them touching the slider.

typedef struct {
    uint32_t est_ma;         // estimated draw of the last frame
    uint32_t peak_ma;        // rolling peak, so a scrolling frame's worst
                             // moment is visible and not averaged away
    uint32_t lit_px;         // non-black pixels in the last frame
    uint32_t px;             // chain length, for the UI's own what-if model
    uint8_t  req_brightness; // brightness before the limiter (setting and cap)
    uint8_t  eff_brightness; // brightness the last frame actually went out at
    bool     limited;        // the limiter clamped the last frame
    bool     floor_exceeded; // quiescent draw alone is over the budget
} led_power_status_t;

void led_driver_set_power_cfg(const power_cfg_t *cfg);
void led_driver_get_power_cfg(power_cfg_t *out);
void led_driver_get_power(led_power_status_t *out);

// ---- freeze ---------------------------------------------------------------
//
// Push one frame at `fixed_brightness` (ignoring the brightness setting, the
// cap and the power limiter) and then send nothing at all until thawed. The
// WS2812B holds its last latched values, so whatever went out stays on the
// panel - across a reboot too, since the LEDs keep their own power.
//
// This exists for firmware updates. The RMT channel runs without DMA and its
// ISR is not IRAM-safe, so it cannot refill the 64-symbol buffer while a
// flash erase or write has the cache disabled. A stall longer than the
// WS2812B's ~50 us reset window makes the strip latch and start a new frame
// mid-stream, so the rest of the bits land at the wrong LED and at the wrong
// bit offset inside each byte - and misalignment turns small channel values
// into large ones. That is both the garbage on the panel and a current spike
// far above what the content should draw.
//
// A frame that is already black cannot be corrupted upward: misaligned zero
// bits are still zero. Freezing is therefore the fix rather than merely a
// mitigation, and it is stronger than lowering the brightness, which leaves
// nonzero bytes to be misread.
//
// The frame buffer is untouched, so thawing and refreshing restores whatever
// was on screen. The marker overlay is not stamped onto the frozen frame: a
// firmware update is not the moment to add current for an indicator.
void led_driver_freeze(uint8_t fixed_brightness);
void led_driver_thaw(void);
bool led_driver_frozen(void);

void led_driver_clear(void);
void led_driver_fill(uint8_t r, uint8_t g, uint8_t b);

void led_driver_set_pixel(int x, int y, uint8_t r, uint8_t g, uint8_t b);
bool led_driver_get_pixel(int x, int y, uint8_t *r, uint8_t *g, uint8_t *b);

// Bypass the mapping and write to a raw strip index. Used by the calibration
// test patterns so the user can see how the physical chain is wired.
void led_driver_set_raw(int strip_index, uint8_t r, uint8_t g, uint8_t b);

// A marker pixel stamped onto every frame as it goes out, on top of whatever
// the renderer drew. Presentation only: the frame buffer underneath is never
// touched, so switching the marker off restores the pixel below it without a
// repaint, and no render path has to know the marker exists.
//
// `brightness` is the marker's own, applied instead of the panel brightness
// and its cap - the offline indicator has to stay readable on a panel dimmed
// by the light sensor or blanked by the schedule, which is the whole point of
// showing it. Out-of-range coordinates (a layout change under a stored
// position) silently draw nothing.
void led_driver_set_marker(bool on, int x, int y,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t brightness);

void led_driver_refresh(void);

void led_driver_lock(void);
void led_driver_unlock(void);
