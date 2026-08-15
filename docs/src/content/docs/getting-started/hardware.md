---
title: Hardware
description: Parts list, level shifter wiring, connectors, and the USB-C power budget
sidebar:
  order: 2
---

![The assembled display: four WS2812B 32x8 panels behind a smoked grey plexiglass front, with a section through the layer stack](../../../assets/display-render.svg)

The reference build is four flexible WS2812B 32x8 panels in a 2x2 grid (64x16 pixels, 1024 LEDs), an ESP32-S3 dev board, one level shifter, and a USB-C socket for power. The drawing above is to the cut sizes in `3D/8x32/`: a 660 by 180 mm sheet, a 10 mm LED pitch, and the twelve 20 by 30 mm cutouts the panel wiring passes through. The firmware itself supports any grid the [arrangement editor](../../admin-guide/arrangement-editor/) can describe: up to 8 rows by 16 columns, 32 panels, 8192 pixels.

The PCB expects **5 V LED panels**: the power rail, the level shifter reference and the connectors are all laid out for 5 V WS2812B-class matrices. 12 V panel variants (WS2815 and similar) need a different power arrangement and are not supported by this board.

## Parts

| Part | Choice | Notes |
|------|--------|-------|
| MCU board | ESP32-S3-DevKitC-1 (ESP32-S3-WROOM-1 N16R8) | 16 MB flash, 8 MB PSRAM, native USB for flashing |
| LED panels | WS2812B matrix, 8x8 / 16x16 / 8x32 | The three sizes commonly sold; flexible or rigid both work |
| Level shifter | SN74HCT125N | 3.3 V logic to the 5 V data level the panels want |
| Series resistor | 330 ohm | Damps reflections on the data run between board and first panel |
| Power input | USB-C breakout, CC1 and CC2 each pulled to GND through 5.1 kohm | Advertises a plain 5 V / 3 A sink, no PD chip needed |
| Connectors | JST-SM on the panels, JST-XH on the PCB | Same 2.5 mm pitch, not cross-compatible: use short pigtail adapters |
| Light sensor | Optional: an LDR plus a 10 kOhm to 100 kOhm resistor, or an analog lux module | Divider output to an ADC1 pin (GPIO1 to GPIO10) lets the room set the brightness, see the [light sensor guide](../../admin-guide/light-sensor/) |

The `PCB/` directory holds the Fritzing source and Gerbers for the controller board (revision v3). `3D/` holds the enclosure parts:

| File | Format | Part |
|------|--------|------|
| `3D/8x32/Front and back panels.dxf` | DXF, laser-cut | The front and back sheets of the panel sandwich, cut twice from smoked grey plexiglass |
| `3D/8x32/Display modules support panel.dxf` | DXF, cut | The aluminium middle panel the LED panels are glued to, with square holes where each panel's wires exit |
| `3D/8x32/Spacers.stl` | STL, printed | Spacers in two lengths: thin between front panel and support panel, long between support panel and back panel |
| `3D/Box for PCB_v2.stl` | STL, printed | Case for the controller board, with openings for the JST connectors |
| `3D/USB type c connector flange.stl` | STL, printed | Mount for the panel-mount USB-C power socket |

Only these exports are tracked; the SketchUp sources the parts were modelled in are not. The [assembly guide](../assembly/) puts the stack together step by step.

## Why the level shifter

WS2812B specifies logic-high as 0.7 x VDD. At a 5 V supply that is 3.5 V, and the ESP32's 3.3 V output sits below it. Some chains tolerate that margin, many glitch. One SN74HCT125 channel fixes it:

| SN74HCT125 pin | Connects to |
|----------------|-------------|
| 1 (1OE) | GND (enables channel 1 permanently) |
| 2 (1A) | ESP32 GPIO 16 (`LED_DATA_GPIO` in the firmware) |
| 3 (1Y) | 330 ohm resistor, then the first panel's DIN |
| 7 (GND) | Common ground |
| 14 (VCC) | 5 V from VBUS |
| Unused xOE pins | Tie to VCC to disable |

## Data chain

Each panel has a 3-pin signal connector (5 V, data, GND) with DIN on one side and DOUT on the other. Chain DOUT to the next panel's DIN in whatever order keeps the cabling short; the arrangement editor adapts the mapping to the order you ended up with, including panels mounted upside down. Feed each panel's power directly from the supply rather than through the daisy-chained signal wires, which are too thin to carry several panels' current.

## Power budget

Numbers measured on the prototype rig on 2026-05-20: one 32x8 panel, fully lit in the stored amber (80, 32, 0) at brightness 255, drew 7.7 W at the USB-C input. Linear extrapolation puts the full 2x2 grid around 31 W at that intensity, which is far past the 15 W a plain USB-C source provides. At the intended dim operating point (brightness 32, displayed colour around (10, 4, 0)) the full grid needs roughly 4 W and a plain USB-C supply is fine.

Two consequences:

- Keep the brightness cap set (Hardware tab). It clamps the slider and every API request, so no automation can push the panel past what the supply survives
- If you want full-brightness white on a large grid, plan a 5 V / 5 A supply negotiated by a PD trigger (CH224K or IP2721) instead of the 5.1 kohm resistor trick

An [ambient light sensor](../../admin-guide/light-sensor/) helps here as well: with the upper end of its brightness range set inside the budget, the display only draws its peak in a room bright enough to need it.

The refresh rate also scales with size: WS2812B needs about 30 microseconds per LED, so 1024 pixels refresh at about 33 Hz and the 8192-pixel maximum at about 4 Hz. Fine for a static readout, visible on fast scrolling.
