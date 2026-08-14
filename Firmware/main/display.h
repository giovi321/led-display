#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "fonts.h"

#define DISPLAY_TEXT_MAX  128

#define FONT_SCALE_MIN    1
#define FONT_SCALE_MAX    8   // upper bound; clamped at runtime to what fits
                              // the active display height

// How the text moves. Stored in NVS as a plain u8; keep the values stable.
typedef enum {
    DISPLAY_SCROLL_OFF        = 0,  // static, centred when it fits
    DISPLAY_SCROLL_RESTART    = 1,  // exits fully, then re-enters (v1 "scroll")
    DISPLAY_SCROLL_CONTINUOUS = 2,  // marquee: the next copy follows after a gap
} display_scroll_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} display_color_t;

esp_err_t display_init(void);

// Set text. `scroll_mode` is a display_scroll_t (unknown values behave as
// RESTART). `scale` is the integer font multiplier (1..FONT_SCALE_MAX); a
// scale of 0 is interpreted as "keep the previous scale", which keeps the
// HTTP API backwards-compatible with clients that don't send the field.
void display_show_text(const char *text,
                       display_color_t color,
                       uint8_t scroll_mode,
                       uint8_t speed,
                       uint8_t scale);

void display_show_text_static(const char *text, display_color_t color);

// Select the active font (FONT_ID_*) and spacing (proportional trims blank
// glyph columns; monospace keeps the fixed cell). Does not repaint on its
// own - callers follow up with display_show_text(), which every mutation
// path already does. A font too tall for the display falls back at render
// time (font_fit), so a layout change can never blank the text.
void display_set_font(uint8_t font_id, bool prop);
void display_get_font(uint8_t *font_id_out, bool *prop_out);

void display_clear(void);

void display_get_state(char *text_out, size_t text_size,
                      display_color_t *color_out,
                      uint8_t *scroll_mode_out,
                      uint8_t *speed_out,
                      uint8_t *scale_out);

// Calibration / diagnostic patterns. While a test is active the scroll task
// stays idle (text is cleared and scrolling stops). Call
// `display_show_text(...)` or `display_clear()` to leave the test mode.
typedef enum {
    DISPLAY_TEST_STEP_RAW,     // walk strip indices 0..255 one at a time (raw, ignores mapping)
    DISPLAY_TEST_STEP_XY,      // walk logical (x,y) 0..31 cols then rows (uses mapping)
    DISPLAY_TEST_CORNERS,      // light the 4 logical corners in distinct colours
    DISPLAY_TEST_BORDER,       // outline of the panel using the mapping
    DISPLAY_TEST_FILL,         // every LED on at the current default amber
} display_test_t;

void display_run_test(display_test_t pattern);

// Identify mode for the arrangement editor: light every physical panel in a
// distinct colour keyed by its chain index (0 red, 1 green, 2 blue, 3 yellow,
// 4 magenta, 5 cyan, 6 orange, 7 white, then repeat brighter/dimmer). The
// web editor paints the drawn panels with the same palette. `on=false`
// restores the stored text.
void display_identify(bool on);

// Synchronous helper: draw a single-pixel border in the given colour. Used by
// the boot splash; does not touch the persisted state.
void display_draw_border(display_color_t color);
