// Host unit tests for mapping.c: plain C11, plain assert, no ESP-IDF.
// Build/run via Firmware/test/host/run_tests.sh (main returns 0 on success).
//
// Coverage:
//  - ORACLE: legacy modes 0..6 reproduce the v1 driver's xy_to_index()
//    exactly (reference copied below from
//    `git show firmware-v2~2:Firmware/main/led_driver.c`)
//  - the real hardware's "rows swapped" arrangement
//  - LUT: every entry filled, unique, in range
//  - 90/270 rotations on square panels; rejection on non-square
//  - validation failures (chain, bounds, pixel cap)
//  - every origin/axis/serpentine combination on a 4x2 panel

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "mapping.h"

// ---------------------------------------------------------------------------
// Reference implementation: the old xy_to_index() switch from the v1 driver
// (git show firmware-v2~2:Firmware/main/led_driver.c), with DISPLAY_WIDTH /
// DISPLAY_HEIGHT and the s_map_mode static turned into parameters.
// ---------------------------------------------------------------------------

#define PANEL_WIDTH  32
#define PANEL_HEIGHT 8

enum {
    LED_MAP_ROW_MAJOR = 0,
    LED_MAP_SERPENTINE,
    LED_MAP_ROW_MAJOR_BL,
    LED_MAP_SERPENTINE_BL,
    LED_MAP_COL_MAJOR,
    LED_MAP_COL_SERPENT,
    LED_MAP_GRID_2X2_COL_SERP,
};

static int oracle_xy_to_index(int mode, int x, int y, int disp_w, int disp_h)
{
    int yy = y;
    int xx = x;

    switch (mode) {
        case LED_MAP_ROW_MAJOR:
            return yy * disp_w + xx;

        case LED_MAP_SERPENTINE:
            return yy * disp_w +
                   ((yy & 1) ? (disp_w - 1 - xx) : xx);

        case LED_MAP_ROW_MAJOR_BL:
            yy = disp_h - 1 - yy;
            return yy * disp_w + xx;

        case LED_MAP_SERPENTINE_BL:
            yy = disp_h - 1 - yy;
            return yy * disp_w +
                   ((yy & 1) ? (disp_w - 1 - xx) : xx);

        case LED_MAP_COL_MAJOR:
            return xx * disp_h + yy;

        case LED_MAP_COL_SERPENT:
            return xx * disp_h +
                   ((xx & 1) ? (disp_h - 1 - yy) : yy);

        case LED_MAP_GRID_2X2_COL_SERP: {
            // 4 panels of 32x8 column-serpentine each, chain order:
            //   chain 0 -> TL (x:0..31,  y:0..7),  mounted upright
            //   chain 1 -> TR (x:32..63, y:0..7),  mounted upright
            //   chain 2 -> BR (x:32..63, y:8..15), mounted rotated 180 degrees
            //   chain 3 -> BL (x:0..31,  y:8..15), mounted rotated 180 degrees
            int panel_col = xx / PANEL_WIDTH;   // 0 = left, 1 = right
            int panel_row = yy / PANEL_HEIGHT;  // 0 = top,  1 = bottom
            int chain_idx;
            if (panel_row == 0) {
                chain_idx = (panel_col == 0) ? 0 : 1;  // TL, TR
            } else {
                chain_idx = (panel_col == 0) ? 3 : 2;  // BL, BR  (chain visits BR before BL)
            }
            int base = chain_idx * PANEL_WIDTH * PANEL_HEIGHT;
            int lx = xx % PANEL_WIDTH;
            int ly = yy % PANEL_HEIGHT;
            // Bottom panels are physically mounted upside-down so the chain
            // wiring stays short. From the panel's perspective its own (0,0)
            // is at the display's far corner of the panel cell.
            if (panel_row == 1) {
                lx = PANEL_WIDTH  - 1 - lx;
                ly = PANEL_HEIGHT - 1 - ly;
            }
            int local = lx * PANEL_HEIGHT +
                        ((lx & 1) ? (PANEL_HEIGHT - 1 - ly) : ly);
            return base + local;
        }

        default:
            return yy * disp_w + xx;
    }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Assert that mapping_xy_to_index over the whole display is a bijection onto
// 0..pixels-1 (every strip index hit exactly once).
static void assert_index_permutation(const mapping_config_t *c)
{
    static bool seen[MAP_MAX_PIXELS];
    const int width  = mapping_width(c);
    const int height = mapping_height(c);
    const int pixels = width * height;

    memset(seen, 0, sizeof(seen));
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int idx = mapping_xy_to_index(c, x, y);
            assert(idx >= 0 && idx < pixels);
            assert(!seen[idx]);
            seen[idx] = true;
        }
    }
}

// Single-panel config builder used by the wiring/rotation tests.
static mapping_config_t single_panel(int w, int h, map_origin_t origin,
                                     map_axis_t axis, bool serpentine,
                                     uint16_t rot)
{
    mapping_config_t c;
    memset(&c, 0, sizeof(c));
    c.panel.w = (uint8_t)w;
    c.panel.h = (uint8_t)h;
    c.panel.origin = origin;
    c.panel.axis = axis;
    c.panel.serpentine = serpentine;
    c.rows = 1;
    c.cols = 1;
    c.cells[0][0].chain = 0;
    c.cells[0][0].rot = rot;
    return c;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Legacy modes 0..6 must reproduce the v1 driver bit-for-bit. The stock v1
// build compiled DISPLAY_WIDTH/HEIGHT as 64x16, and modes 0..5 walked that
// whole surface, so they migrate as one 64x16 pseudo-panel; mode 6 is the
// real 2x2 grid of 32x8 modules.
static void test_legacy_oracle(void)
{
    for (int mode = 0; mode <= 6; mode++) {
        mapping_config_t c;
        assert(mapping_from_legacy(mode, &c) == MAPPING_OK);
        assert(mapping_validate(&c) == MAPPING_OK);

        const int width  = mapping_width(&c);
        const int height = mapping_height(&c);
        assert(width == 64 && height == 16);

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                assert(mapping_xy_to_index(&c, x, y) ==
                       oracle_xy_to_index(mode, x, y, width, height));
            }
        }
    }

    // Unknown modes are rejected.
    mapping_config_t c;
    assert(mapping_from_legacy(7, &c) == MAPPING_ERR_BOUNDS);
    assert(mapping_from_legacy(-1, &c) == MAPPING_ERR_BOUNDS);

    // The factory default is legacy mode 6.
    mapping_config_t def, mode6;
    mapping_default(&def);
    assert(mapping_from_legacy(6, &mode6) == MAPPING_OK);
    assert(memcmp(&def, &mode6, sizeof(def)) == 0);

    printf("test_legacy_oracle: OK\n");
}

// The hardware as mounted today: the panel pairs swapped top/bottom relative
// to legacy mode 6. Logically that shifts every pixel by half the height, so
// new(x,y) must equal oracle mode 6 at (x, (y+8) mod 16).
static void test_rows_swapped(void)
{
    mapping_config_t c;
    assert(mapping_from_legacy(6, &c) == MAPPING_OK);
    c.cells[0][0] = (panel_cell_t){ .chain = 3, .rot = 180 };
    c.cells[0][1] = (panel_cell_t){ .chain = 2, .rot = 180 };
    c.cells[1][0] = (panel_cell_t){ .chain = 0, .rot = 0 };
    c.cells[1][1] = (panel_cell_t){ .chain = 1, .rot = 0 };
    assert(mapping_validate(&c) == MAPPING_OK);

    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 64; x++) {
            assert(mapping_xy_to_index(&c, x, y) ==
                   oracle_xy_to_index(LED_MAP_GRID_2X2_COL_SERP,
                                      x, (y + 8) % 16, 64, 16));
        }
    }

    printf("test_rows_swapped: OK\n");
}

// mapping_build_lut must fill every entry with a unique in-range index.
static void test_lut(void)
{
    static uint16_t lut[MAP_MAX_PIXELS];
    static bool seen[MAP_MAX_PIXELS];

    mapping_config_t c;
    assert(mapping_from_legacy(6, &c) == MAPPING_OK);
    const int pixels = mapping_pixels(&c);

    // Poison the buffer so "entry never written" cannot masquerade as a
    // valid index.
    memset(lut, 0xFF, sizeof(lut));
    assert(mapping_build_lut(&c, lut) == MAPPING_OK);

    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < pixels; i++) {
        assert(lut[i] < pixels);   // also catches the 0xFFFF poison
        assert(!seen[lut[i]]);
        seen[lut[i]] = true;
    }

    // And the LUT agrees with the per-pixel function.
    const int width = mapping_width(&c);
    for (int y = 0; y < mapping_height(&c); y++) {
        for (int x = 0; x < width; x++) {
            assert(lut[y * width + x] == mapping_xy_to_index(&c, x, y));
        }
    }

    // Invalid configs must be rejected before any write.
    c.cells[0][0].chain = c.cells[0][1].chain;
    assert(mapping_build_lut(&c, lut) == MAPPING_ERR_CHAIN);

    printf("test_lut: OK\n");
}

// 90/270 rotations: accepted on square panels (indices stay a permutation),
// rejected on non-square ones.
static void test_rotation_square(void)
{
    static const uint16_t rots[] = { 0, 90, 180, 270 };

    for (size_t i = 0; i < sizeof(rots) / sizeof(rots[0]); i++) {
        mapping_config_t c = single_panel(8, 8, MAP_ORIGIN_TL, MAP_AXIS_ROW,
                                          true, rots[i]);
        assert(mapping_validate(&c) == MAPPING_OK);
        assert_index_permutation(&c);
    }

    // A 2x1 grid of square panels with mixed 90/270 also stays a bijection.
    mapping_config_t grid = single_panel(8, 8, MAP_ORIGIN_BL, MAP_AXIS_COL,
                                         true, 90);
    grid.cols = 2;
    grid.cells[0][1] = (panel_cell_t){ .chain = 1, .rot = 270 };
    assert(mapping_validate(&grid) == MAPPING_OK);
    assert_index_permutation(&grid);

    // 90 degrees on a non-square 32x8 panel is a rotation error.
    mapping_config_t bad = single_panel(32, 8, MAP_ORIGIN_TL, MAP_AXIS_COL,
                                        true, 90);
    assert(mapping_validate(&bad) == MAPPING_ERR_ROT);
    bad.cells[0][0].rot = 270;
    assert(mapping_validate(&bad) == MAPPING_ERR_ROT);

    // Rotations that are not multiples of 90 are always errors.
    bad.cells[0][0].rot = 45;
    assert(mapping_validate(&bad) == MAPPING_ERR_ROT);

    printf("test_rotation_square: OK\n");
}

static void test_validation_failures(void)
{
    mapping_config_t c;

    // Duplicate chain index.
    assert(mapping_from_legacy(6, &c) == MAPPING_OK);
    c.cells[1][0].chain = 2;  // duplicates cells[1][1]
    assert(mapping_validate(&c) == MAPPING_ERR_CHAIN);

    // Chain index out of range (4 panels -> valid chains are 0..3).
    assert(mapping_from_legacy(6, &c) == MAPPING_OK);
    c.cells[0][0].chain = 4;
    assert(mapping_validate(&c) == MAPPING_ERR_CHAIN);

    // 0x0 panel.
    c = single_panel(1, 1, MAP_ORIGIN_TL, MAP_AXIS_ROW, false, 0);
    c.panel.w = 0;
    c.panel.h = 0;
    assert(mapping_validate(&c) == MAPPING_ERR_BOUNDS);

    // Panel dimension above 64.
    c = single_panel(1, 1, MAP_ORIGIN_TL, MAP_AXIS_ROW, false, 0);
    c.panel.w = 65;
    assert(mapping_validate(&c) == MAPPING_ERR_BOUNDS);

    // Grid dimensions out of range.
    c = single_panel(8, 8, MAP_ORIGIN_TL, MAP_AXIS_ROW, false, 0);
    c.rows = 0;
    assert(mapping_validate(&c) == MAPPING_ERR_BOUNDS);
    c.rows = MAP_MAX_ROWS + 1;
    assert(mapping_validate(&c) == MAPPING_ERR_BOUNDS);
    c.rows = 1;
    c.cols = MAP_MAX_COLS + 1;
    assert(mapping_validate(&c) == MAPPING_ERR_BOUNDS);

    // Pixel cap exceeded: 8 rows x 16 cols of 16x16 (also blows the panel
    // cap; either way it must come back as a bounds error).
    c = single_panel(16, 16, MAP_ORIGIN_TL, MAP_AXIS_ROW, false, 0);
    c.rows = 8;
    c.cols = 16;
    for (int r = 0; r < 8; r++) {
        for (int col = 0; col < 16; col++) {
            c.cells[r][col].chain = (uint8_t)(r * 16 + col);
        }
    }
    assert(mapping_validate(&c) == MAPPING_ERR_BOUNDS);

    // Pixel cap alone: 2x16 of 64x16 is exactly 32 panels (allowed) but
    // 32768 pixels (not allowed).
    c = single_panel(64, 16, MAP_ORIGIN_TL, MAP_AXIS_ROW, false, 0);
    c.rows = 2;
    c.cols = 16;
    for (int r = 0; r < 2; r++) {
        for (int col = 0; col < 16; col++) {
            c.cells[r][col].chain = (uint8_t)(r * 16 + col);
        }
    }
    assert(mapping_validate(&c) == MAPPING_ERR_BOUNDS);

    // Exactly at both caps is fine: 2x16 of 16x16 = 32 panels, 8192 pixels.
    c = single_panel(16, 16, MAP_ORIGIN_TL, MAP_AXIS_ROW, false, 0);
    c.rows = 2;
    c.cols = 16;
    for (int r = 0; r < 2; r++) {
        for (int col = 0; col < 16; col++) {
            c.cells[r][col].chain = (uint8_t)(r * 16 + col);
        }
    }
    assert(mapping_validate(&c) == MAPPING_OK);
    assert(mapping_pixels(&c) == MAP_MAX_PIXELS);

    printf("test_validation_failures: OK\n");
}

// Every origin/axis/serpentine combination on a 4x2 panel: the 8 offsets are
// a permutation of 0..7, and the first-LED corner always maps to offset 0.
static void test_wiring_combos(void)
{
    // Logical coordinates of each origin corner on an unrotated 4x2 panel.
    static const struct { int x, y; } corner[4] = {
        [MAP_ORIGIN_TL] = { 0, 0 },
        [MAP_ORIGIN_TR] = { 3, 0 },
        [MAP_ORIGIN_BL] = { 0, 1 },
        [MAP_ORIGIN_BR] = { 3, 1 },
    };

    for (int origin = MAP_ORIGIN_TL; origin <= MAP_ORIGIN_BR; origin++) {
        for (int axis = MAP_AXIS_ROW; axis <= MAP_AXIS_COL; axis++) {
            for (int serp = 0; serp <= 1; serp++) {
                mapping_config_t c = single_panel(4, 2,
                                                  (map_origin_t)origin,
                                                  (map_axis_t)axis,
                                                  serp != 0, 0);
                assert(mapping_validate(&c) == MAPPING_OK);
                assert_index_permutation(&c);
                assert(mapping_xy_to_index(&c, corner[origin].x,
                                           corner[origin].y) == 0);
            }
        }
    }

    printf("test_wiring_combos: OK\n");
}

int main(void)
{
    test_legacy_oracle();
    test_rows_swapped();
    test_lut();
    test_rotation_square();
    test_validation_failures();
    test_wiring_combos();

    printf("test_mapping: all tests passed\n");
    return 0;
}
