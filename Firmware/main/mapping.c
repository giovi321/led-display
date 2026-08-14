// Runtime pixel-mapping engine. Pure C11, no ESP-IDF, no allocation, so the
// whole module runs on the host under plain gcc + assert (see
// Firmware/test/host/test_mapping.c). Mapping math is where this project has
// historically burned time, hence the isolation.
//
// Pipeline for one logical pixel (x,y), (0,0) top-left, x right, y down:
//   1. cell lookup:     (row,col) = (y/h, x/w), local (lx,ly) = (x%w, y%h)
//   2. inverse rotation: undo the clockwise mounting rotation -> (px,py)
//   3. origin transform: move (0,0) to the first-LED corner -> (wx,wy)
//   4. wiring walk:      row- or column-first, optionally serpentine -> offset
//   5. chain placement:  index = chain * w*h + offset

#include "mapping.h"

#include <string.h>

int mapping_validate(const mapping_config_t *c)
{
    if (!c) {
        return MAPPING_ERR_BOUNDS;
    }

    if (c->panel.w < 1 || c->panel.w > 64 || c->panel.h < 1 || c->panel.h > 64) {
        return MAPPING_ERR_BOUNDS;
    }
    // origin/axis arrive from decoded JSON in the API layer; reject values
    // outside the enums instead of indexing garbage later.
    const int origin = (int)c->panel.origin;
    const int axis   = (int)c->panel.axis;
    if (origin < MAP_ORIGIN_TL || origin > MAP_ORIGIN_BR ||
        axis < MAP_AXIS_ROW || axis > MAP_AXIS_COL) {
        return MAPPING_ERR_BOUNDS;
    }
    if (c->rows < 1 || c->rows > MAP_MAX_ROWS ||
        c->cols < 1 || c->cols > MAP_MAX_COLS) {
        return MAPPING_ERR_BOUNDS;
    }

    const int panels = mapping_panels(c);
    if (panels > MAP_MAX_PANELS) {
        return MAPPING_ERR_BOUNDS;
    }
    if (mapping_pixels(c) > MAP_MAX_PIXELS) {
        return MAPPING_ERR_BOUNDS;
    }

    // Every chain index 0..panels-1 must appear exactly once: with `panels`
    // cells, "all in range and no duplicates" is exactly a permutation.
    bool seen[MAP_MAX_PANELS] = { false };
    for (int r = 0; r < c->rows; r++) {
        for (int col = 0; col < c->cols; col++) {
            const panel_cell_t *cell = &c->cells[r][col];

            if (cell->rot != 0 && cell->rot != 90 &&
                cell->rot != 180 && cell->rot != 270) {
                return MAPPING_ERR_ROT;
            }
            if ((cell->rot == 90 || cell->rot == 270) && c->panel.w != c->panel.h) {
                return MAPPING_ERR_ROT;
            }

            if (cell->chain >= panels || seen[cell->chain]) {
                return MAPPING_ERR_CHAIN;
            }
            seen[cell->chain] = true;
        }
    }

    return MAPPING_OK;
}

int mapping_xy_to_index(const mapping_config_t *c, int x, int y)
{
    const int w = c->panel.w;
    const int h = c->panel.h;
    const panel_cell_t *cell = &c->cells[y / h][x / w];
    const int lx = x % w;
    const int ly = y % h;

    // rot is the CLOCKWISE rotation applied when the panel was mounted, so
    // recovering the panel's own frame needs the INVERSE rotation. 90/270
    // are only legal for square panels (validated), where w == h.
    int px, py;
    switch (cell->rot) {
        case 90:  px = ly;         py = w - 1 - lx; break;
        case 180: px = w - 1 - lx; py = h - 1 - ly; break;
        case 270: px = h - 1 - ly; py = lx;         break;
        default:  px = lx;         py = ly;         break;  // rot 0
    }

    // Wiring frame: origin at the first-LED corner so the walk below always
    // starts at (0,0) regardless of which corner the strip enters.
    int wx = px;
    int wy = py;
    switch (c->panel.origin) {
        case MAP_ORIGIN_TR: wx = w - 1 - px;                  break;
        case MAP_ORIGIN_BL:                  wy = h - 1 - py; break;
        case MAP_ORIGIN_BR: wx = w - 1 - px; wy = h - 1 - py; break;
        case MAP_ORIGIN_TL:
        default:                                              break;
    }

    int offset;
    if (c->panel.axis == MAP_AXIS_ROW) {
        offset = wy * w + ((c->panel.serpentine && (wy & 1)) ? (w - 1 - wx) : wx);
    } else {
        offset = wx * h + ((c->panel.serpentine && (wx & 1)) ? (h - 1 - wy) : wy);
    }

    return (int)cell->chain * (w * h) + offset;
}

int mapping_build_lut(const mapping_config_t *c, uint16_t *lut)
{
    if (!lut) {
        return MAPPING_ERR_BOUNDS;
    }
    const int err = mapping_validate(c);
    if (err != MAPPING_OK) {
        return err;
    }

    const int width  = mapping_width(c);
    const int height = mapping_height(c);
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            // Fits in uint16_t: mapping_validate caps pixels at 8192.
            lut[y * width + x] = (uint16_t)mapping_xy_to_index(c, x, y);
        }
    }
    return MAPPING_OK;
}

int mapping_from_legacy(int legacy_mode, mapping_config_t *out)
{
    if (!out) {
        return MAPPING_ERR_BOUNDS;
    }

    // Zero first so unused cells are deterministic (the config is persisted
    // and compared as a whole blob).
    memset(out, 0, sizeof(*out));

    // Modes 0..5 described a wiring walk across the WHOLE v1 display surface,
    // which the stock v1 build compiled as 64x16. Model that as one 64x16
    // pseudo-panel so a migrated device lights every LED exactly like v1 did
    // (cells[0][0] stays {chain 0, rot 0} from the memset above). Mode 6
    // overrides the panel to the real 32x8 modules below.
    out->panel.w = 64;
    out->panel.h = 16;
    out->rows = 1;
    out->cols = 1;

    switch (legacy_mode) {
        case 0:  // LED_MAP_ROW_MAJOR
            out->panel.origin = MAP_ORIGIN_TL;
            out->panel.axis   = MAP_AXIS_ROW;
            out->panel.serpentine = false;
            break;
        case 1:  // LED_MAP_SERPENTINE
            out->panel.origin = MAP_ORIGIN_TL;
            out->panel.axis   = MAP_AXIS_ROW;
            out->panel.serpentine = true;
            break;
        case 2:  // LED_MAP_ROW_MAJOR_BL
            out->panel.origin = MAP_ORIGIN_BL;
            out->panel.axis   = MAP_AXIS_ROW;
            out->panel.serpentine = false;
            break;
        case 3:  // LED_MAP_SERPENTINE_BL
            out->panel.origin = MAP_ORIGIN_BL;
            out->panel.axis   = MAP_AXIS_ROW;
            out->panel.serpentine = true;
            break;
        case 4:  // LED_MAP_COL_MAJOR
            out->panel.origin = MAP_ORIGIN_TL;
            out->panel.axis   = MAP_AXIS_COL;
            out->panel.serpentine = false;
            break;
        case 5:  // LED_MAP_COL_SERPENT
            out->panel.origin = MAP_ORIGIN_TL;
            out->panel.axis   = MAP_AXIS_COL;
            out->panel.serpentine = true;
            break;
        case 6:  // LED_MAP_GRID_2X2_COL_SERP
            // Chain walks TL -> TR -> BR -> BL; the bottom row is mounted
            // upside-down (rot 180) to keep the chain cabling short.
            out->panel.w = 32;   // real 32x8 modules, not the 64x16 pseudo-panel
            out->panel.h = 8;
            out->panel.origin = MAP_ORIGIN_TL;
            out->panel.axis   = MAP_AXIS_COL;
            out->panel.serpentine = true;
            out->rows = 2;
            out->cols = 2;
            out->cells[0][0] = (panel_cell_t){ .chain = 0, .rot = 0 };
            out->cells[0][1] = (panel_cell_t){ .chain = 1, .rot = 0 };
            out->cells[1][0] = (panel_cell_t){ .chain = 3, .rot = 180 };
            out->cells[1][1] = (panel_cell_t){ .chain = 2, .rot = 180 };
            break;
        default:
            return MAPPING_ERR_BOUNDS;
    }

    return MAPPING_OK;
}

void mapping_default(mapping_config_t *out)
{
    // Factory default matches the shipped hardware: legacy mode 6, the 2x2
    // grid of 32x8 column-serpentine panels.
    (void)mapping_from_legacy(6, out);
}
