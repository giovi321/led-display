---
title: Overview
description: What the project is, what lives in the repository, and what firmware v2 can do
sidebar:
  order: 1
---

led-display is firmware for a wall display made of daisy-chained WS2812B matrix panels, driven by an ESP32-S3. The display shows one number or short message at a time, pushed to it over HTTP by whatever you like: a home automation flow, a cron job, a plain `curl`. Typical uses are a revenue figure, a subscriber counter, or a temperature that should always be visible somewhere.

The panel configuration is a runtime model rather than something compiled in: you describe the physical build in a graphical editor and the firmware rebuilds its pixel mapping on the fly.

![The reference build: four WS2812B 32x8 panels behind a smoked grey plexiglass front, with a section through the layer stack](../../../assets/display-render.svg)

The drawing is to the cut geometry in `3D/8x32/`: a 670 by 190 mm sheet with twelve perimeter fixings, a 10 mm LED pitch, and the wire cutouts through the support panel. The lit pixels are drawn with the firmware's own font, so they are the ones the device would light.

## What is in the repository

| Directory | Content |
|-----------|---------|
| `Firmware/` | ESP-IDF v6 application: C sources, the embedded web UI, the vendored WireGuard component, host-side unit tests, and a WSL build helper |
| `PCB/` | Fritzing source and exported Gerbers for the controller board (current revision v4) |
| `3D/` | Enclosure parts as DXF outlines to cut and STL to print: under `8x32/` the front and back panels, the LED panel support panel and the spacers of a 32x8 build, plus the controller case and the USB-C connector flange. The SketchUp sources are not tracked, only the exports |
| `docs/` | This documentation site (Astro Starlight) |

## What the firmware does

The display side is deliberately simple. It renders text in one colour at a time, static or scrolling, in one of three pixel fonts (5x7, a tiny 3x5, a tall 7x12 with real lowercase) at integer sizes, proportionally spaced with fixed-width digits. The web UI covers the routine controls: text, a colour picker plus up to 12 saved colours kept as one-tap chips, brightness, text size and spacing, scroll mode and speed. A weekly schedule turns the display off, or holds it at a dim level, at set times per weekday, using NTP-synced time. An optional BH1750 lux meter on the board's I2C connector can set the brightness from the room instead.

The arrangement editor (Hardware tab, advanced mode) describes the physical build instead of hardcoding it. You pick one panel type, with 8x8, 16x16, and 8x32 presets or custom sizes up to 64x64 pixels, set the wiring (first-LED corner, row or column walk, serpentine), and place panels on a grid of up to 8 rows by 16 columns, capped at 32 panels and 8192 pixels. Drag a panel onto another cell to swap them, tap a panel to rotate it, and use identify mode to light every physical panel in a distinct colour so you can match the drawing to the wall. Applying a layout validates it, rebuilds the pixel mapping live, and persists it.

The display shows one value, and it gets that value three ways. The device can fetch it from a URL on an interval, picking the field out of a JSON response if needed. `POST /api/value` accepts a raw number or string. An optional MQTT client takes values over a broker, and can announce itself to Home Assistant. Every source goes through the same formatting (multiplier, scaling to thousands or millions, decimals, separators, prefix and suffix) before showing it. A fallback reacts when the feed breaks, meaning no new value for a set time, repeated failed polls or a lost broker: it replaces the readout with a text, marks a corner pixel or dims the display, and can notify a webhook.

On the network side the device keeps up to five WiFi networks, joins the strongest one in range at boot, retries a lost link with a growing backoff, and falls back to its own setup network when nothing answers. One device name sets the hostname, that setup network's SSID, and the name the device reports in webhooks.

Operationally, firmware updates go over the air after the initial USB flash: upload a `.bin` from the browser or have the device pull one from a URL, with two OTA slots and automatic rollback if a new image fails to come up. The whole configuration exports as one JSON file and imports back, WiFi credentials deliberately excluded. An optional API key gates every mutating request behind an `X-Api-Key` header. Outbound webhooks POST device events (boot, IP change, OTA result, schedule on and off, value stale and recovered) and an optional status heartbeat to one configurable URL, consumable by anything that accepts a webhook. Finally, an optional WireGuard client (compiled in by default, inactive until configured) covers remote access without port-forwarding the device.

## Where to go next

- [Hardware](../hardware/) for the parts list, wiring, and power budget
- [Assembly](../assembly/) to build the reference display from the cut and printed parts
- [First flash](../first-flash/) to build the firmware and flash it over USB
- [WiFi setup](../wifi-setup/) to put the display on your network
