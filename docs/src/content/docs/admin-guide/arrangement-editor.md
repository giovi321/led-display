---
title: Arrangement editor
description: Match the firmware's pixel mapping to the panels as they actually hang on the wall, by dragging a drawing until it looks like reality
sidebar:
  order: 2
---

The arrangement editor is a graphical tool in the Hardware tab ([advanced mode](../advanced-mode/)) that describes the physical build to the firmware: how many panels there are, how each one is wired internally, which chain position hangs where, and which panels are mounted rotated.

It exists because assembly rarely matches the plan. Panels get mounted in a different order than they were soldered, or upside down, and once the wiring is potted in silicone there is no cheap way to fix it physically. The wiring is fixed; the software adapts. Instead of editing mapping code and reflashing, you drag a drawing of the panels until it matches the wall, press Apply, and the firmware rebuilds its pixel mapping on the spot.

![Mapping pipeline from logical pixel to strip index](/led-display/assets/diagrams/mapping-pipeline.svg)

## Concepts

The mapping model has two parts, a panel type and a layout.

The **panel type** describes one physical panel model. All panels in a display are identical. Four properties describe how the LED strip snakes through a panel, together called the panel's wiring:

| Property | Values | Meaning |
|----------|--------|---------|
| w, h | 1 to 64 | pixels per panel, for example 32x8 |
| origin | TL, TR, BL, BR | corner where the first LED of the strip sits |
| axis | row or col | whether the strip walks rows or columns first |
| serpentine | on/off | direction alternates every row or column |

The **layout** is a grid of rows x cols cells. A cell is one grid position, and each cell holds two things:

- a chain index: the panel's position in the physical data daisy-chain, 0 for the panel wired directly to the controller, 1 for the next, and so on. Chain indices form a permutation: every index appears in exactly one cell
- a rotation: 0 or 180 degrees for any panel; 90 and 270 additionally allowed for square panels (w equals h)

The logical display size follows from the layout: width is cols times w, height is rows times h. On every layout change the firmware rebuilds a lookup table that maps each logical pixel straight to its strip index, so the mapping costs nothing per frame.

![Chain order and per-panel wiring](/led-display/assets/diagrams/wiring-chain.svg)

## The workflow

1. Turn on advanced mode and open the Hardware tab. The editor loads the active layout from the device and draws it to scale: each panel as a rounded rectangle with its chain index in a badge, a dot on the corner where its first LED sits, a chip showing how the panel is mounted (0, 90, 180 or 270 degrees), and a line running from chain 0 to the last panel so the cable path is visible at a glance
2. Press **Identify**. Every physical panel lights up in a colour keyed to its chain index: chain 0 red, 1 green, 2 blue, 3 yellow, 4 magenta, 5 cyan, 6 orange, 7 white. Chains 8 to 15 repeat the same hues at half brightness, 16 and up at quarter brightness. The editor paints the drawn panels in the same colours
3. Compare the drawing with the wall. Where the colours disagree, drag a drawn panel onto another cell to swap the two. Where a panel is mounted upside down, tap it to rotate (rectangular panels toggle 0 and 180; square panels cycle 0, 90, 180, 270). Repeat until the drawing matches reality
4. Press **Apply**. The firmware validates the layout (bounds, chain permutation, rotation legality), rebuilds the lookup table live, saves the layout to flash, re-renders the stored text, and runs the border test pattern so you get immediate visual confirmation of the new edges
5. If the border does not trace the outline you expect, the panel wiring is probably wrong rather than the arrangement; see the test patterns below

Identify mode ends when you leave the Hardware tab, and any test pattern also switches it off. The editor shows an "unsaved changes" banner whenever the drawing differs from what the device last acknowledged; Revert reloads the device's layout.

## Grid, presets, and limits

The Grid steppers change rows and cols. Shrinking the grid drops the removed cells and compacts the surviving chain indices to 0..n-1 in their existing order; growing assigns the next free chain indices to the new cells. Either way the layout stays a valid permutation, so you can resize freely and fix the order afterwards with Identify and drag.

The Panel row has preset buttons for 8x8, 16x16, and 8x32, the sizes WS2812B matrices are actually sold in, plus custom width and height fields for anything else from 1x1 to 64x64. Switching to a non-square panel size snaps any 90/270 rotation to the nearest legal value.

The Wiring row describes one panel rather than the arrangement, in the words you would use looking at the panel itself: First LED (the corner the strip enters, the four buttons draw the dot where it would sit), Strip runs (in rows or in columns), and Zigzag (the strip reverses on every row or column instead of jumping back to the same side, which is how almost every off-the-shelf matrix is built). Each label carries a `?` that explains the setting, and a small preview beside them draws the strip path through one panel for the current selection.

Limits, enforced by the firmware and mirrored by the editor's disabled buttons:

| Limit | Value |
|-------|-------|
| Grid rows | 8 |
| Grid cols | 16 |
| Panels | 32 |
| Total pixels | 8192 |

The pixel cap matters because the whole chain is one serial line: WS2812B data takes about 30 microseconds per LED, and every frame rewrites every LED. 1024 pixels refresh at about 33 Hz; the 8192 pixel maximum refreshes at about 4 Hz. That is fine for a static readout and visibly choppy for scrolling text. The editor footer shows the resulting display size, panel count, and estimated refresh rate for the current layout.

## Test patterns

The Test patterns card fires one-shot calibration patterns. Each answers a different question:

| Pattern | What it shows | Use it to |
|---------|---------------|-----------|
| Step strip | lights one LED at a time in physical chain order, ignoring the mapping | see where the chain starts and how the strip walks each panel; this is the ground truth for choosing origin, axis, and serpentine |
| Step (x,y) | lights one logical pixel at a time, left to right, top to bottom, through the active mapping | verify the mapping walks the display the way you expect |
| Corners | logical (0,0) red, (W-1,0) green, (0,H-1) blue, (W-1,H-1) yellow | check orientation at a glance: red must be top-left as you face the display |
| Border | a one-pixel frame around the logical edge | confirm the display edge is where the mapping thinks it is; runs automatically after Apply |
| Fill | every pixel lit | find dead pixels and sanity-check the power supply under full load |

Patterns run in the background and clear the current text; sending new text or clearing the display returns to normal operation.

## Legacy preset API

The v1 endpoint `POST /api/display/map` still works. It accepts `{"mode":"..."}` with the v1 mode names (`row`, `serp`, `row_bl`, `serp_bl`, `col`, `col_serp`, `grid_2x2_col_serp`) and installs the equivalent layout, so existing automations keep working. New clients should read and write the full layout via `GET`/`POST /api/layout` instead.

If an [API key](../api-key/) is set, Apply and the test buttons need it like every other POST; the web UI handles that automatically. Once the arrangement finally matches the wall, [export a config backup](../backup-restore/) so you never have to do this dance again.
