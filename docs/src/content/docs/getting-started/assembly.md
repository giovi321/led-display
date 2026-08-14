---
title: Assembly
description: Build the reference display as a five-layer sandwich from the cut and printed parts in 3D/
sidebar:
  order: 3
---

The reference display is a sandwich held together by twelve screws that run through the whole stack. The LED panels are glued to an aluminium panel in the middle, a smoked grey plexiglass sheet covers the front, and a second sheet closes the back. All the cut and printed parts come from `3D/`, listed in the [hardware guide](../hardware/).

This page covers the physical build. It assumes the [parts](../hardware/#parts) are on the bench and the controller board is finished. Nothing here has to be done before flashing, so if you would rather see the firmware run first, do the [first flash](../first-flash/) on a bare panel and come back.

## The stack

Front to back:

1. Front panel, smoked grey plexiglass, laser-cut from `Front and back panels.dxf`
2. Thin printed spacers from `Spacers.stl`, holding the front panel off the LED panels
3. Support panel, aluminium, cut from `Display modules support panel.dxf`, with the LED panels glued to its front face
4. Long printed spacers from the same `Spacers.stl`, leaving room behind the support panel for the wiring
5. Back panel, plexiglass, cut from `Front and back panels.dxf` again

Twelve M5x40 flat-head socket screws go in from the front, pass through all five layers, and close on twelve M5 hex acorn cap nuts at the back. The 40 mm screw length is what fixes the total stack height, so the two sheet thicknesses plus the two spacer lengths have to leave enough thread for the nuts.

The controller board is not part of the sandwich. It lives in its own printed case, `Box for PCB_v2.stl`, which leaves the JST connectors accessible so the panel pigtails plug straight in.

## Parts

| Part | Quantity | From | Notes |
|------|----------|------|-------|
| Front panel | 1 | `3D/8x32/Front and back panels.dxf` | Smoked grey plexiglass, laser-cut |
| Back panel | 1 | same file | Same sheet, same cut |
| Support panel | 1 | `3D/8x32/Display modules support panel.dxf` | Aluminium, with square holes for the LED panel wires |
| Thin spacers | as needed | `3D/8x32/Spacers.stl` | Printed, front panel to support panel |
| Long spacers | as needed | same file | Printed, support panel to back panel |
| M5x40 flat-head socket screws | 12 | hardware shop | Inserted from the front |
| M5 hex acorn cap nuts | 12 | hardware shop | Closed at the back |
| LED panels | 4 | see [hardware](../hardware/) | WS2812B 32x8, wires already soldered |
| Silicone adhesive | as needed | hardware shop | Glues the LED panels to the support panel |
| Controller case | 1 | `3D/Box for PCB_v2.stl` | Printed, JST connectors exposed |
| USB-C socket | 1 | electronics shop | Panel-mount type with a screw fixing plate, [example listing](https://www.aliexpress.com/item/1005005995278252.html) |
| USB-C flange | 1 | `3D/USB type c connector flange.stl` | Printed, holds the socket |

The cut files are the authority on geometry: they define the panel positions and the twelve screw positions, and they are what tells you how many spacers of each length to print. Open them rather than measuring from this page.

## Order of work

1. Send `Front and back panels.dxf` to be laser-cut twice from plexiglass and `Display modules support panel.dxf` to be cut once from aluminium. Print both spacer lengths from `Spacers.stl`, the controller case, and the USB-C flange
2. Dry-fit the stack with two screws: check the holes line up and the screws still take the nuts. Flat-head screws only sit flush in a countersunk hole, which a laser will not cut, so chamfer the twelve holes in the front panel now if you want the heads level with the surface
3. Lay the support panel front face up and put each LED panel over its position, feeding its pre-soldered wires through the square hole behind it. Check the orientation of every panel and the reach of every pigtail before any glue goes on: once the panels are siliconed down, nothing about their position or rotation can be changed
4. Bond the LED panels to the support panel with silicone and let it cure fully before handling the panel further. Silicone stays flexible, which matters because the aluminium and the flexible panels do not expand at the same rate
5. Behind the support panel, chain each panel's DOUT to the next panel's DIN and run the first panel's DIN to the controller. Feed panel power from the supply rather than through the daisy-chained signal wires; the [hardware guide](../hardware/#data-chain) covers why. Note the order you end up chaining them in, but do not force a particular order: the [arrangement editor](../../admin-guide/arrangement-editor/) adapts to whatever order the cabling turned out to be
6. Mount the panel-mount USB-C socket in the printed flange with its screw fixing plate and run its 5 V and GND leads to the controller board
7. Close the sandwich: lay the front panel face down, drop the twelve screws through it, then add the thin spacers, the populated support panel, the long spacers, and the back panel. Thread the acorn nuts on and tighten them evenly, in a criss-cross pattern rather than one edge at a time. Plexiglass cracks around over-torqued holes, so stop at hand-tight
8. Close the controller board in its printed case and connect the panel pigtails and the power leads through the openings

## First power-up

Power the display and follow [first flash](../first-flash/) and [WiFi setup](../wifi-setup/) if you have not already. Then open the web UI, go to the Hardware tab in [advanced mode](../../admin-guide/advanced-mode/), and use the [arrangement editor](../../admin-guide/arrangement-editor/):

1. Press Identify. Every physical panel lights in its own colour
2. Drag the on-screen panels until the drawing matches the colours on the wall, and rotate any panel that was mounted upside down
3. Apply. The firmware rebuilds its pixel mapping and stores it

The silicone fixes the panel positions and orientations for good, and the chain order is fixed by the cabling behind them. The mapping is the part that adapts, which is why a panel glued in rotated or chained out of order costs nothing to correct.

Keep the brightness cap set on the Hardware tab. The [power budget](../../architecture/power/) explains what the reference build draws and why a plain USB-C supply cannot feed it at full brightness.
