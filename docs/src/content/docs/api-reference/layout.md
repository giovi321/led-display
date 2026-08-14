---
title: Layout
description: GET and POST /api/layout, the full panel-arrangement JSON, validation rules and limits
sidebar:
  order: 3
---

The layout describes the whole physical build: one panel type (all panels identical) plus a grid of cells saying which chain index sits in which grid position and how that panel is rotated. The graphical arrangement editor in the web UI reads and writes exactly this shape; a script can do the same.

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/api/layout` | no | Read the active layout, derived size and editor limits |
| POST | `/api/layout` | yes | Replace the layout (full document, not a partial update) |

## GET /api/layout

```bash
curl http://192.168.1.50/api/layout
```

Response for a 2x2 grid of 32x8 panels:

```json
{
  "v": 1,
  "panel": {"w": 32, "h": 8, "origin": "TL", "axis": "col", "serpentine": true},
  "grid": {"rows": 2, "cols": 2},
  "cells": [
    {"row": 0, "col": 0, "chain": 0, "rot": 0},
    {"row": 0, "col": 1, "chain": 1, "rot": 0},
    {"row": 1, "col": 0, "chain": 3, "rot": 180},
    {"row": 1, "col": 1, "chain": 2, "rot": 180}
  ],
  "width": 64,
  "height": 16,
  "limits": {"max_rows": 8, "max_cols": 16, "max_panels": 32, "max_pixels": 8192}
}
```

`width`, `height` and `limits` are derived or informational; they exist so the editor can bound its inputs without hardcoding firmware constants. They are ignored on POST, as is `v`.

## Field semantics

| Field | Values | Meaning |
|---|---|---|
| `panel.w`, `panel.h` | 1..64 | Pixels per panel, horizontal and vertical |
| `panel.origin` | `TL`, `TR`, `BL`, `BR` | Corner of the panel where the first LED of the strip sits, as seen with the panel mounted unrotated |
| `panel.axis` | `row`, `col` | Whether the strip walks a full row then moves to the next row, or a full column then the next column |
| `panel.serpentine` | bool | Alternate direction every row or column (zigzag wiring) |
| `grid.rows`, `grid.cols` | 1..8, 1..16 | Grid dimensions; rows times cols is the panel count, at most 32 |
| `cells[].row`, `cells[].col` | within the grid | Which cell this entry describes |
| `cells[].chain` | 0..panels-1 | Position of this panel in the data daisy-chain; across all cells the values must be a permutation (each index exactly once) |
| `cells[].rot` | 0, 90, 180, 270 | Rotation of the panel in degrees clockwise; 90 and 270 are only valid when the panel is square (`w == h`) |

The wiring fields (`origin`, `axis`, `serpentine`) apply to every panel; per-panel differences are expressed only through `chain` and `rot`.

## POST /api/layout

Send the same document. Include one entry per grid cell: unlisted cells default to `chain 0, rot 0`, which fails the permutation check on any grid larger than 1x1. The body limit is 4096 bytes, enough for the maximum 32 panels.

On success the firmware applies the layout to the driver, persists it, re-renders the stored text onto the new geometry and runs the border test pattern so you get visual confirmation of the new edges. If persisting fails the layout still applies until reboot and a warning is logged; the request does not fail.

```bash
curl -X POST http://192.168.1.50/api/layout \
  -H "X-Api-Key: mysecret" -H "Content-Type: application/json" \
  -d '{
    "panel": {"w": 32, "h": 8, "origin": "TL", "axis": "col", "serpentine": true},
    "grid": {"rows": 1, "cols": 2},
    "cells": [
      {"row": 0, "col": 0, "chain": 0, "rot": 0},
      {"row": 0, "col": 1, "chain": 1, "rot": 0}
    ]
  }'
```

Response: `{"ok": true}`

## Validation errors

All validation failures are 400 with one of these messages:

| Error message | Cause |
|---|---|
| `panel, grid and cells required` | One of the three top-level members is missing or the wrong type |
| `panel w/h must be numbers 1..64` | `w` or `h` missing, non-numeric or outside 0..255 at parse time |
| `panel origin must be TL, TR, BL or BR` | Unknown or missing origin string |
| `panel axis must be row or col` | Unknown or missing axis string |
| `panel serpentine must be a bool` | Missing or non-boolean |
| `grid rows/cols must be numbers` | Missing or non-numeric grid dimensions |
| `each cell needs numeric row, col and chain` | A cell entry is malformed |
| `cell row/col outside the grid` | A cell entry points outside `grid.rows` x `grid.cols` |
| `cell rot must be 0, 90, 180 or 270` | `rot` missing, non-numeric, negative or above 270 |
| `rot must be 0/90/180/270; 90/270 only on square panels` | `rot` passed parsing but is not a multiple of 90, or is 90/270 on a non-square panel |
| `chain indices must be a permutation of 0..n-1` | A chain index is out of range or duplicated |
| `rows/cols/panel size out of range or over the pixel cap` | Bounds check failed: panel 1..64 per side, grid 1..8 rows and 1..16 cols, at most 32 panels and 8192 pixels total |

A 500 `"reconfigure failed"` means validation passed but the LED driver could not apply the config (for example, out of memory for the pixel buffer).

## Finding your wiring

Use [`POST /api/display/test`](../display/#post-apidisplaytest) with `step_raw` to discover the chain order and per-panel wiring, and [`POST /api/display/identify`](../display/#post-apidisplayidentify) to match physical panels to chain indices while editing.
