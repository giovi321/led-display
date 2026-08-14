#pragma once

// Runtime pixel-mapping model. Replaces the old compile-time led_map_mode_t
// enum with a generic description of the physical build: one panel type
// (all panels identical) plus a grid of cells that says which chain position
// sits where and how it is rotated.
//
// This module is PURE C (no ESP-IDF, no FreeRTOS, no cJSON) so it can be
// unit-tested on the host with plain gcc. JSON (de)serialisation lives in
// the API layer; NVS persistence lives in settings.c.

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Corner of the panel where the first LED of the strip sits, as seen when
// the panel is mounted unrotated ("upright").
typedef enum {
    MAP_ORIGIN_TL = 0,
    MAP_ORIGIN_TR = 1,
    MAP_ORIGIN_BL = 2,
    MAP_ORIGIN_BR = 3,
} map_origin_t;

// Whether the strip snakes along rows or along columns first.
typedef enum {
    MAP_AXIS_ROW = 0,   // walks a full row, then moves to the next row
    MAP_AXIS_COL = 1,   // walks a full column, then moves to the next column
} map_axis_t;

// One physical panel model. Covers every common WS2812B matrix: the
// AliExpress standards are 8x8, 16x16 and 8x32; anything else fits via
// custom w/h. The current hardware is 32x8, origin TL, axis COL, serpentine.
typedef struct {
    uint8_t      w;           // pixels per panel, horizontal, 1..64
    uint8_t      h;           // pixels per panel, vertical,   1..64
    map_origin_t origin;      // corner of the first LED
    map_axis_t   axis;        // row-first or column-first walk
    bool         serpentine;  // alternate direction every row/column
} panel_type_t;

// One grid cell: which chain position is physically mounted there and how
// it is rotated. rot is degrees clockwise: 0 or 180 for any panel; 90/270
// only valid when the panel is square (w == h).
typedef struct {
    uint8_t  chain;  // 0..panel_count-1, a permutation across all cells
    uint16_t rot;    // 0, 90, 180, 270
} panel_cell_t;

#define MAP_MAX_ROWS    8
#define MAP_MAX_COLS    16
#define MAP_MAX_PANELS  32
#define MAP_MAX_PIXELS  8192

typedef struct {
    panel_type_t panel;
    uint8_t      rows;   // 1..MAP_MAX_ROWS
    uint8_t      cols;   // 1..MAP_MAX_COLS, rows*cols <= MAP_MAX_PANELS
    panel_cell_t cells[MAP_MAX_ROWS][MAP_MAX_COLS];  // [row][col]
} mapping_config_t;

// Logical display size derived from the config.
static inline int mapping_width(const mapping_config_t *c)  { return c->cols * c->panel.w; }
static inline int mapping_height(const mapping_config_t *c) { return c->rows * c->panel.h; }
static inline int mapping_pixels(const mapping_config_t *c) { return mapping_width(c) * mapping_height(c); }
static inline int mapping_panels(const mapping_config_t *c) { return c->rows * c->cols; }

// Full structural validation: bounds, pixel cap, chain permutation,
// rotation legality (90/270 only for square panels). Returns 0 on success,
// a negative MAPPING_ERR_* code otherwise.
#define MAPPING_OK               0
#define MAPPING_ERR_BOUNDS      -1   // rows/cols/w/h out of range or pixel cap hit
#define MAPPING_ERR_CHAIN       -2   // chain indices are not a permutation
#define MAPPING_ERR_ROT         -3   // 90/270 on a non-square panel, or not a multiple of 90
int mapping_validate(const mapping_config_t *c);

// Strip index for one logical pixel. Coordinates must be in range and the
// config valid; behaviour is undefined otherwise (the LUT builder is the
// only intended caller, plus the host tests).
int mapping_xy_to_index(const mapping_config_t *c, int x, int y);

// Fill `lut` (mapping_pixels() entries, caller-allocated) with the strip
// index of every logical pixel, row-major: lut[y*width + x]. Validates
// first; returns mapping_validate()'s code.
int mapping_build_lut(const mapping_config_t *c, uint16_t *lut);

// Presets for the legacy led_map_mode_t values (0..6) so stored settings
// keep working. Returns MAPPING_OK or MAPPING_ERR_BOUNDS for unknown modes.
// Legacy modes 0..5 describe a single 32x8 panel with the given wiring;
// mode 6 is the 2x2 grid (chain TL->TR->BR->BL, bottom row rot=180).
int mapping_from_legacy(int legacy_mode, mapping_config_t *out);

// The factory-default config: currently the 2x2 grid, i.e. legacy mode 6.
void mapping_default(mapping_config_t *out);

#ifdef __cplusplus
}
#endif
