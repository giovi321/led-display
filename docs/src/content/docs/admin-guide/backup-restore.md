---
title: Backup and restore
description: Export the whole device configuration as one JSON file and restore it section by section, on the same device or a replacement
sidebar:
  order: 6
---

The device exports its entire persisted configuration as a single JSON document and can restore from the same file. Both live in the System tab ([advanced mode](../advanced-mode/)) under Backup and restore, or at `GET /api/config/export` and `POST /api/config/import`.

## What the export contains

One file, `led-display-config.json`, holding:

- display settings: text, colour, brightness and brightness cap, scroll mode, speed, font, size, spacing, boot screen
- the layout, exactly as the [arrangement editor](../arrangement-editor/) saved it
- saved colour presets
- value endpoint configuration: prefix, suffix, decimals, thousands separator, staleness settings
- integrations: the device name, webhook URL and event mask, and the API key in cleartext
- WireGuard configuration, including the private key in cleartext
- the on/off schedule, including the off-window action and its dim level
- the [light sensor](../light-sensor/) configuration: its I2C pins, both calibration readings, the brightness range and the curve. The live reading is not part of it, since a backup describes the configuration and not what the room looked like

WiFi credentials are deliberately excluded. A config file tends to move between devices and people, and it should not carry the network password along; a restored device joins the network with whatever credentials it already has.

Because the export does contain the API key and the WireGuard private key, the export endpoint itself requires the [API key](../api-key/) once one is set, and the file should be stored like a secret, not committed to a public repo.

## The file is plain JSON

No binary blob, no checksum magic. You can read it, diff two exports, or edit a value by hand before restoring. One dialect note for hand editors: `webhook_events` is stored as a numeric bitmask in the file, while the live `/api/integrations` endpoint speaks event name arrays.

## Restore semantics

Restore applies the file section by section: `layout`, `display`, `light_sensor`, `colors`, `value_config`, `integrations`, `wireguard`, `schedule`. The layout goes first so a restored text renders onto the restored geometry. Sections absent from the file are left untouched, so a hand-trimmed file works as a partial restore and a file from an older build restores cleanly, leaving whatever it does not mention alone. A section that fails validation is skipped without aborting the rest, and the response says exactly what happened:

```json
{"ok": true, "applied": ["layout", "display", "schedule"], "skipped": ["wireguard"]}
```

The web UI shows the same applied/skipped report after a restore. Everything applies live, so no reboot is needed.

Four things to be aware of when restoring:

- a file whose `integrations` section carries an API key turns key checking on the moment it is applied; clients (including other browsers) must present that key from then on
- restoring a `wireguard` section with `enabled: true` starts the tunnel with the restored keys
- a `device_name` in the `integrations` section renames the device: hostname, setup network, and the `device` field in webhooks all follow it, so two devices restored from the same file end up with the same name
- a `light_sensor` section with `enabled: true` hands the brightness to the sensor as soon as it is applied; a section naming an I2C pin the module cannot spare, putting SDA and SCL on the same pin, or whose two calibration readings are equal, lands in `skipped` instead. Pins are validated rather than clamped, so a file from a board wired differently does not silently move the sensor to another pin

## When to export

Export right after you finish the [panel arrangement](../arrangement-editor/). The layout is the setting that costs actual effort to reproduce, since it encodes physical reality that was discovered panel by panel with identify colours. With a backup in hand, a replacement device or a factory reset is a two-minute restore instead of a repeat of that session. Factory reset wipes display settings but keeps WiFi credentials, so a post-reset device is immediately reachable for the restore.
