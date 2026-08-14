---
title: Pixel mapping
description: How the mapping engine turns logical coordinates into chain positions, and how to debug a new panel arrangement
sidebar:
  order: 2
---

The mapping engine (`Firmware/main/mapping.c`) answers one question: given a logical pixel (x, y), which LED on the physical data chain is it? Everything else (font rendering, scrolling, test patterns) draws in logical coordinates and never knows how the panels are wired. This page explains the model and the transform pipeline in enough detail to debug a new arrangement.

Terminology used throughout: a panel is one physical LED matrix module, a cell is one position in the grid of panels, the chain index is a panel's position in the data daisy-chain (0 = first panel after the controller), the layout is the whole arrangement config, and the wiring is how the strip snakes through one panel (origin, axis, serpentine).

## The model

A layout (`mapping_config_t` in `mapping.h`) consists of:

- one panel type shared by all panels: width and height in pixels (1..64 each), plus the wiring (origin corner of the first LED, row-first or column-first axis, serpentine flag)
- a grid of rows x cols cells (up to 8 rows, 16 columns, 32 panels, 8192 pixels total)
- per cell: the chain index mounted there and its rotation in degrees clockwise (0 or 180 for any panel, 90 or 270 only for square panels)

`mapping_validate` enforces all of it structurally: bounds, the pixel cap, rotation legality, and that the chain indices form a permutation of 0..panels-1 (each chain position used exactly once). The API rejects anything invalid with a 400 before the driver is touched.

## From logical pixel to chain position

`mapping_xy_to_index` runs five steps for a pixel (x, y), with (0, 0) top left, x right, y down:

1. Cell lookup: the cell is (y / h, x / w), the local coordinate inside it is (x % w, y % h)
2. Inverse rotation: the cell's `rot` is the clockwise rotation applied when the panel was mounted, so the engine applies the inverse rotation to recover the panel's own frame
3. Origin transform: mirror the coordinate so (0, 0) sits at the corner where the first LED of the strip enters the panel
4. Wiring walk: walk row-first or column-first, reversing every other row or column when serpentine, giving an offset 0..w*h-1 inside the panel
5. Chain placement: strip index = chain * w*h + offset

Worked example on the factory-default layout (2x2 grid of 32x8 panels, column-serpentine, origin top left, bottom row mounted at 180 degrees with chain order TL 0, TR 1, BR 2, BL 3). Take the display's bottom-left pixel (0, 15):

- cell (1, 0): chain 3, rot 180; local coordinate (0, 7)
- inverse of 180 degrees: (31, 0)
- origin is top left, so no mirror: (31, 0)
- column walk, column 31 is odd so serpentine reverses it: offset = 31 * 8 + 7 = 255
- strip index = 3 * 256 + 255 = 1023, the last LED of the chain

## Why a LUT

The five steps above run zero times per frame. At init and on every layout change, `mapping_build_lut` evaluates them once per pixel into a `uint16_t` table (`lut[y * width + x]` = strip index), after validating the config. From then on `led_driver_set_pixel` is a pure table lookup; the per-frame cost of any layout, however contorted, is identical. The 8192-pixel cap keeps the LUT at 16 KB or less, and `uint16_t` is guaranteed sufficient because indices stay below the cap.

## The 16 wiring combinations

A panel's wiring is one of 4 origins (TL, TR, BL, BR) x 2 axes (row-first, column-first) x serpentine on or off: 16 combinations. Two invariants hold for every combination, and the host tests assert them on a 4x2 panel:

- the walk visits every pixel exactly once (the offsets are a permutation)
- the origin corner always maps to offset 0, so lighting strip index 0 physically shows you which corner the strip enters

That second invariant is the practical debugging tool: if you do not know a panel's wiring, run the raw step test (below) and watch where index 0 lights up and which direction the walk moves.

## Debugging a new arrangement

The workflow that works, using the web UI's Hardware tab (the arrangement editor, `web/editor.js`) and the calibration test patterns:

1. Run the raw strip walk (`step` raw pattern): it lights strip indices 0..n one at a time, bypassing the mapping entirely, so you see the physical chain order and each panel's wiring direction
2. Set the panel type in the editor: size, origin corner, axis, serpentine, based on what the walk showed
3. Use identify mode: the firmware lights every panel in a distinct color keyed by its chain index (0 red, 1 green, 2 blue, 3 yellow, ...), and the editor paints its drawn cells with the same palette, so mismatches between the drawing and reality are visible at a glance
4. Drag cells and set rotations in the editor until the drawing matches the wall
5. Apply: `POST /api/layout` validates, rebuilds the LUT, re-renders the stored text under the new layout, and runs the border pattern so you get immediate visual confirmation of the display edges

If the border is broken at panel boundaries, a cell has the wrong chain index or rotation. If text is mirrored or upside down within one panel, the wiring (origin or axis) is wrong. If entire panels show shifted content, the serpentine flag or panel size is wrong.

## Legacy mode migration

Firmware v1 stored a single integer mapping mode (NVS key `map`, values 0..6). On the first v2 boot, `mapping_from_legacy` converts it to an equivalent layout and persists it as the `layout` blob; the legacy key is never consulted again. Modes 0..5 describe one 32x8 panel with the six wirings v1 supported (row/column, top-left or bottom-left origin, serpentine or not); mode 6 is the 2x2 grid with the chain running TL, TR, BR, BL and the bottom row mounted at 180 degrees. Mode 6 is also the factory default for a blank device, matching the shipped hardware.

## Host tests as the safety net

Mapping math is where this project has historically burned time, which is why the engine is pure C11 with no ESP-IDF dependency: the whole module runs on the host under plain gcc and `assert` (`Firmware/test/host/test_mapping.c`, run via `run_tests.sh`, see [building](../../development/building/)).

The anchor is an oracle test: the v1 driver's `xy_to_index` switch statement was copied verbatim into the test file, and every legacy mode 0..6 is checked pixel by pixel against it. The v2 engine must reproduce v1 bit for bit, so the migration cannot silently scramble an existing display. The remaining tests cover the real hardware's rows-swapped arrangement, LUT completeness (every entry written, unique, in range), 90/270 rotations on square panels and their rejection on non-square ones, all validation failure paths, and the permutation and origin invariants across all 16 wiring combinations.

The engine's place in the wider firmware is described in [firmware architecture](../firmware/).
