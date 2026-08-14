#pragma once

#include <stdbool.h>
#include <stdint.h>

// Pixel fonts for the LED text renderer, printable ASCII 32..126. Column
// major: one entry per column, bit 0 = top row; fonts taller than 8 rows
// store uint16_t columns. PURE C - no ESP-IDF - so the host tests under
// Firmware/test/host/ can compile this module with plain gcc.

#define FONT_ID_5X7   0   // default; the fw 2.x renderer's original font
#define FONT_ID_3X5   1   // compact, uppercase-only letterforms
#define FONT_ID_7X12  2   // tall, real lowercase with descenders
#define FONT_COUNT    3

typedef struct {
    const char *name;     // API name: "5x7", "3x5", "7x12"
    uint8_t     w;        // glyph cell width in columns
    uint8_t     h;        // glyph height in rows
    uint8_t     digit_w;  // fixed advance for digits (tabular figures)
    uint8_t     space_w;  // advance of the space character (proportional)
    const void *data;     // [95][w] of uint8_t (h<=8) or uint16_t columns
} font_def_t;

// Always returns a valid font; unknown ids fall back to FONT_ID_5X7.
const font_def_t *font_get(uint8_t id);

// Id for an API name; -1 when unknown.
int font_id_by_name(const char *name);
const char *font_name(uint8_t id);

// `id` if that font fits in `max_h` rows, else the tallest font that does,
// else the shortest font overall.
uint8_t font_fit(uint8_t id, int max_h);

// Column bits of a glyph; characters outside 32..126 render as '?'.
uint16_t font_glyph_col(const font_def_t *f, char c, int col);

// Horizontal span of a glyph: first column and width in columns.
// Proportional: blank edge columns are trimmed; digits always advance
// digit_w so a changing value keeps a stable width; space advances space_w.
// Monospace: every glyph spans the full cell (start 0, width w).
void font_glyph_span(const font_def_t *f, char c, bool prop,
                     int *start, int *width);

// Rendered width in pixels of `s` at integer `scale`, including one
// scaled column of gap between glyphs but no trailing gap.
int font_text_width(const font_def_t *f, const char *s, bool prop, int scale);
