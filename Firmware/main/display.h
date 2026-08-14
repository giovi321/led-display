#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define FONT_WIDTH        5
#define FONT_HEIGHT       7
#define FONT_CELL_WIDTH   6   // 5 cols of glyph + 1 col gap (at scale 1)

#define DISPLAY_TEXT_MAX  128

#define FONT_SCALE_MIN    1
#define FONT_SCALE_MAX    8   // upper bound; clamped at runtime to what fits
                              // the active display height

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} display_color_t;

esp_err_t display_init(void);

// Set text. `scale` is the integer font multiplier (1..FONT_SCALE_MAX). A
// scale of 0 is interpreted as "keep the previous scale", which keeps the
// HTTP API backwards-compatible with clients that don't send the field.
void display_show_text(const char *text,
                       display_color_t color,
                       bool scroll,
                       uint8_t speed,
                       uint8_t scale);

void display_show_text_static(const char *text, display_color_t color);

void display_clear(void);

void display_get_state(char *text_out, size_t text_size,
                      display_color_t *color_out,
                      bool *scroll_out,
                      uint8_t *speed_out,
                      uint8_t *scale_out);

// Calibration / diagnostic patterns. While a test is active the scroll task
// stays idle (text is cleared and `scroll` becomes false). Call
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
