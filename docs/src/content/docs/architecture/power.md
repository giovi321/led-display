---
title: Power budget
description: Measured power draw, USB-C limits, and the firmware brightness cap that keeps the display inside them
sidebar:
  order: 3
---

The reference build is 1024 WS2812B LEDs (a 2x2 grid of 32x8 panels) powered from a single USB-C input at 5 V. The intended use is a dim, always-on readout of one value or a short message, and the power design leans on that: dim operation fits default USB-C with margin, full brightness does not.

## USB-C limits

Standard USB-C without PD negotiation provides 5 V at 3 A (15 W). The board requests that by pulling CC1 and CC2 to GND through 5.1 kOhm resistors each; no PD chip is involved. With a PD trigger IC (CH224K, IP2721) the input could negotiate up to 5 A at 5 V (25 W), which is the upgrade path if a future mode needs bright output across all panels.

## Measured draw

Measured on 2026-05-20 on the prototype rig, single panel, before the full grid was assembled: 256 LEDs all lit at the stored amber (R=80, G=32, B=0) with brightness=255 (full unscaled output) drew 7.7 W at the USB-C input.

Everything below is extrapolation from that one measurement, not measured on the assembled grid:

- linear extrapolation to 4 panels at the same intensity gives roughly 31 W, well over the 15 W default USB-C ceiling, so a fully lit rectangle at brightness 255 is not survivable on a non-PD source
- at the intended operating point (brightness=32, displayed color around R=10, G=4, B=0) the same scaling gives roughly 1 W per panel, about 4 W for the full grid, comfortably inside default USB-C

The ESP32-S3 itself adds an estimated 0.5 W (estimate from the project notes, not separately measured).

Open item: re-measure on the assembled 4-panel grid in both regimes, brightness=32 (operating point) and brightness=255 (worst case), and replace the projections above with actual figures.

## Brightness cap as defence in depth

The 15 W ceiling is enforced in firmware, not just by convention. The LED driver applies two values on every refresh: the user brightness (slider or API, 0..255, default 32) and a separate brightness cap on top of it. The effective brightness is the user setting clamped by the cap, so a power limit configured once survives careless API calls, a user dragging the slider to 255, or a bug that writes a large brightness value. A cap of 255 means uncapped.

Both values persist in NVS and are restored at boot in the safe order: the cap is applied before the brightness, so an over-cap stored value never reaches the panels even for one frame. The cap is settable from the advanced section of the web UI; the brightness is the ordinary slider (`POST /api/display/brightness`).

The cap is defence in depth, not a calculated current limiter: it scales RGB values, and actual draw depends on content and color. Set it based on measurement for your supply, or leave the default operating point (brightness 32) and it never matters.

Where the cap sits in the code and the boot sequence is covered in [firmware architecture](../firmware/).
