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
- value configuration: prefix, suffix, decimals, multiplier, scale and scale letters, decimal and thousands separators, plus sign, and the [fallback](../../user-guide/value-updates/#fallback) text, mode and triggers
- the URL poll (`value_poll`): enabled flag, URL, interval, value path, and the secret and custom header value in cleartext
- integrations: the device name, webhook URL and event mask, the heartbeat interval, and the API key, webhook secret and webhook header value in cleartext
- MQTT: broker URI, credentials in cleartext, base topic and Home Assistant discovery settings
- WireGuard configuration, including the private key in cleartext
- the on/off schedule, including the off-window action and its dim level
- the [light sensor](../light-sensor/) configuration: its I2C pins, both calibration readings, the brightness range and the curve. The live reading is not part of it, since a backup describes the configuration and not what the room looked like

WiFi credentials are deliberately excluded. A config file tends to move between devices and people, and it should not carry the network password along; a restored device joins the network with whatever credentials it already has.

Because the export does contain the API key, the poll and webhook secrets, the MQTT password and the WireGuard private key, the export endpoint itself requires the [API key](../api-key/) once one is set, and the file should be stored like a secret, not committed to a public repo. A poll URL that carries credentials is exported as written.

## The file is plain JSON

No binary blob, no checksum magic. You can read it, diff two exports, or edit a value by hand before restoring. One dialect note for hand editors: `webhook_events` is stored as a numeric bitmask in the file, while the live `/api/integrations` endpoint speaks event name arrays.

## Restore semantics

Restore applies the file section by section: `layout`, `display`, `light_sensor`, `colors`, `value_config`, `integrations`, `value_poll`, `wireguard`, `mqtt`, `schedule`. The layout goes first so a restored text renders onto the restored geometry. Sections absent from the file are left untouched, so a hand-trimmed file works as a partial restore and a file from an older build restores cleanly, leaving whatever it does not mention alone. A section that fails validation is skipped without aborting the rest, and the response says exactly what happened:

```json
{"ok": true, "applied": ["layout", "display", "schedule"], "skipped": ["wireguard"]}
```

The web UI shows the same applied/skipped report after a restore. Everything applies live, so no reboot is needed.

A backup from 2.11.0 or earlier restores through the same conversion the device runs on upgrade: its `value_config` section carries the old `stale_hours`, `stale_action`, `stale_text` and `default_text` keys, which become the fallback text, mode and timeout, and a file without `value_poll` leaves the poll settings alone. The mapping is in [upgrading from 2.11.0](../../user-guide/value-updates/#upgrading-from-2110). A backup written by 2.12.0 carries the new keys only. Restoring a `value_poll` or `integrations` section wakes the poller and the heartbeat, so the new settings take effect at once.

Five things to be aware of when restoring:

- a file whose `integrations` section carries an API key turns key checking on the moment it is applied; clients (including other browsers) must present that key from then on
- a `value_poll` section with `enabled: true` starts polling its URL with the restored secret straight away, and a section whose URL or header fails validation lands in `skipped`
- restoring a `wireguard` section with `enabled: true` starts the tunnel with the restored keys
- a `device_name` in the `integrations` section renames the device: hostname, setup network, and the `device` field in webhooks all follow it, so two devices restored from the same file end up with the same name
- a `light_sensor` section with `enabled: true` hands the brightness to the sensor as soon as it is applied; a section naming an I2C pin the module cannot spare, putting SDA and SCL on the same pin, or whose two calibration readings are equal, lands in `skipped` instead. Pins are validated rather than clamped, so a file from a board wired differently does not silently move the sensor to another pin

## When to export

Export right after you finish the [panel arrangement](../arrangement-editor/). The layout is the setting that costs actual effort to reproduce, since it encodes physical reality that was discovered panel by panel with identify colours. With a backup in hand, a replacement device or a factory reset is a two-minute restore instead of a repeat of that session. Factory reset wipes display settings but keeps WiFi credentials, so a post-reset device is immediately reachable for the restore.
