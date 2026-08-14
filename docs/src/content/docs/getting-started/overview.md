---
title: Overview
description: What the project is, what lives in the repository, and what firmware v2 can do
sidebar:
  order: 1
---

led-display is firmware for a wall display made of daisy-chained WS2812B matrix panels, driven by an ESP32-S3. The display shows one number or short message at a time, pushed to it over HTTP by whatever you like: a home automation flow, a cron job, a plain `curl`. Typical uses are a revenue figure, a subscriber counter, or a temperature that should always be visible somewhere.

Firmware v2 (version 2.0.0) replaces the compile-time panel configuration of v1 with a runtime model: you describe the physical build in a graphical editor and the firmware rebuilds its pixel mapping on the fly.

:::note[Photo pending]
A photo of the reference build (four 32x8 panels in a 2x2 grid, 64x16 pixels) goes here once one is taken
:::

## What is in the repository

| Directory | Content |
|-----------|---------|
| `Firmware/` | ESP-IDF v6 application: C sources, the embedded web UI, the vendored WireGuard component, host-side unit tests, and a WSL build helper |
| `PCB/` | Fritzing source and exported Gerbers for the controller board (current revision v3) |
| `3D/` | Printable enclosure parts as SketchUp sources and STL exports: panel frames, spacers, PCB box, USB-C connector flange |
| `docs/` | This documentation site (Astro Starlight) |

## What the firmware does

The display side is deliberately simple. It renders static or scrolling text in a 5x7 font at integer scales, in one colour at a time. The web UI covers the routine controls: text, a colour picker plus up to 12 saved colours kept as one-tap chips, brightness, scroll speed, and font size. A weekly schedule turns the display off and on at set times per weekday, using NTP-synced time.

The arrangement editor (Hardware tab, advanced mode) describes the physical build instead of hardcoding it. You pick one panel type, with 8x8, 16x16, and 8x32 presets or custom sizes up to 64x64 pixels, set the wiring (first-LED corner, row or column walk, serpentine), and place panels on a grid of up to 8 rows by 16 columns, capped at 32 panels and 8192 pixels. Drag a panel onto another cell to swap them, tap a panel to rotate it, and use identify mode to light every physical panel in a distinct colour so you can match the drawing to the wall. Applying a layout validates it, rebuilds the pixel mapping live, and persists it.

For pushing data there is a dedicated value endpoint: `POST /api/value` accepts a raw number or string, and the firmware formats it through a configurable template (prefix, suffix, decimals, thousands separator) before showing it. A staleness guard reacts when no new value has arrived for a configured number of hours: dim the display, blink a corner pixel, or replace the readout with a warning text.

Operationally, firmware updates go over the air after the initial USB flash: upload a `.bin` from the browser or have the device pull one from a URL, with two OTA slots and automatic rollback if a new image fails to come up. The whole configuration exports as one JSON file and imports back, WiFi credentials deliberately excluded. An optional API key gates every mutating request behind an `X-Api-Key` header. Outbound webhooks POST device events (boot, IP change, OTA result, schedule on and off, stale value) to one configurable URL, consumable by anything that accepts a webhook. Finally, an optional WireGuard client (compiled in by default, inactive until configured) covers remote access without port-forwarding the device.

## Where to go next

- [Hardware](../hardware/) for the parts list, wiring, and power budget
- [First flash](../first-flash/) to build the firmware and flash it over USB
- [WiFi setup](../wifi-setup/) to put the display on your network
