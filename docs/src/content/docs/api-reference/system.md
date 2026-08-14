---
title: System
description: Status, reboot, boot screen, factory reset, schedule, WiFi, OTA updates and config export/import
sidebar:
  order: 5
---

Everything that keeps the device running: the status tree the web UI polls, power-cycle style operations, the on/off schedule, WiFi provisioning, firmware updates and whole-config backup. Auth follows the [shared rules](../): POSTs need `X-Api-Key` when a key is set, GETs are open except `/api/config/export`.

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/api/status` | no | Full device status |
| POST | `/api/system/reboot` | yes | Reboot |
| POST | `/api/system/boot_screen` | yes | What to show right after boot |
| POST | `/api/system/factory_reset` | yes | Wipe settings, keep WiFi |
| GET | `/api/schedule` | no | Read the on/off schedule |
| POST | `/api/schedule` | yes | Update the schedule (partial update) |
| GET | `/api/wifi/scan` | no | Scan for networks |
| POST | `/api/wifi/connect` | yes, except in AP setup mode | Save credentials and reboot |
| POST | `/api/wifi/reset` | yes, except in AP setup mode | Clear credentials and reboot into setup |
| POST | `/api/ota/upload` | yes | Flash a firmware image from the request body |
| POST | `/api/ota/url` | yes | Pull a firmware image from a URL |
| GET | `/api/ota/status` | no | Update progress and last result |
| GET | `/api/config/export` | yes | Download the whole config as one JSON file |
| POST | `/api/config/import` | yes | Restore a config document |

## GET /api/status

The one call that answers "what is this device doing right now". The web UI polls it; a monitoring script can too.

```bash
curl http://192.168.1.50/api/status
```

```json
{
  "wifi": {
    "mode": "sta",
    "ssid": "MyNetwork",
    "ip": "192.168.1.50",
    "rssi": -52,
    "connected": true
  },
  "display": {
    "text": "$1,234,567.89",
    "scroll": false,
    "speed": 50,
    "scale": 1,
    "brightness": 60,
    "color": {"r": 80, "g": 32, "b": 0}
  },
  "system": {
    "fw": "2.0.0",
    "chip": "ESP32",
    "cores": 2,
    "rev": 3,
    "heap": 123456,
    "uptime": 3600,
    "brightness_cap": 128,
    "boot_screen": "ip"
  },
  "layout": {
    "rows": 2, "cols": 2,
    "panel_w": 32, "panel_h": 8,
    "width": 64, "height": 16,
    "panels": 4
  },
  "value_age_s": 120,
  "wireguard": {
    "compiled_in": true,
    "enabled": false,
    "up": false,
    "last_handshake": 0
  },
  "ota": {
    "slot": "ota_0",
    "version": "2.0.0",
    "in_progress": false
  },
  "schedule_active": true
}
```

| Field | Meaning |
|---|---|
| `wifi.mode` | `sta` (joined a network), `ap` (setup portal) or `boot` (still deciding) |
| `wifi.rssi` | Signal strength in dBm |
| `display.*` | Current text state; `speed` and `scale` as documented under [display control](../display/#post-apidisplaytext) |
| `system.heap` | Free heap in bytes |
| `system.uptime` | Seconds since boot |
| `system.boot_screen` | `text`, `ip` or `none` |
| `layout.*` | Summary of the active layout; the full config comes from [`GET /api/layout`](../layout/) |
| `value_age_s` | Seconds since the last `POST /api/value`; -1 means never |
| `wireguard.last_handshake` | Unix seconds, 0 means never |
| `ota.slot` | Running partition: `factory`, `ota_0` or `ota_1` |
| `schedule_active` | `true` when the schedule currently allows the display to be on |

## POST /api/system/reboot

Replies `{"ok": true}`, then reboots 500 ms later so the response gets out. No body needed.

```bash
curl -X POST http://192.168.1.50/api/system/reboot -H "X-Api-Key: mysecret"
```

## POST /api/system/boot_screen

Chooses what the display shows right after boot.

| Field | Type | Meaning |
|---|---|---|
| `mode` | string | `text` (the stored text), `ip` (the device's IP address) or `none` |

```bash
curl -X POST http://192.168.1.50/api/system/boot_screen \
  -H "X-Api-Key: mysecret" -d '{"mode": "ip"}'
```

Errors: 400 `"unknown mode"`.

## POST /api/system/factory_reset

Erases all display settings (text, layout, colors, value config, integrations, WireGuard, schedule) and reboots. WiFi credentials live in a separate NVS namespace and survive on purpose, so a factory reset cannot strand a wall-mounted device off the network; use `/api/wifi/reset` for that.

```bash
curl -X POST http://192.168.1.50/api/system/factory_reset -H "X-Api-Key: mysecret"
```

```json
{"ok": true, "note": "display settings wiped; wifi kept"}
```

Errors: 500 `"erase failed"`.

## GET /api/schedule

```bash
curl http://192.168.1.50/api/schedule
```

```json
{
  "enabled": true,
  "on_hour": 8, "on_min": 0,
  "off_hour": 22, "off_min": 30,
  "days": 31,
  "utc_offset": 60
}
```

| Field | Meaning |
|---|---|
| `days` | Bitmask, bit 0 = Monday through bit 6 = Sunday; 127 = every day, 31 = weekdays |
| `utc_offset` | Minutes from UTC, for example 60 for CET |
| `on_hour`/`on_min`, `off_hour`/`off_min` | Local switch-on and switch-off times; a window that wraps midnight (on 22:00, off 06:00) works |

## POST /api/schedule

Partial update: only fields present in the body change. The handler does not range-check; keep hours in 0..23, minutes in 0..59, `days` in 0..127 and `utc_offset` in -840..840 minutes yourself.

```bash
curl -X POST http://192.168.1.50/api/schedule \
  -H "X-Api-Key: mysecret" \
  -d '{"enabled":true,"on_hour":8,"off_hour":22,"days":127,"utc_offset":60}'
```

Response: `{"ok": true}`. Schedule transitions fire the `schedule_on` / `schedule_off` [webhooks](../integrations/#outbound-webhooks).

## GET /api/wifi/scan

Returns up to 20 visible networks.

```bash
curl http://192.168.1.50/api/wifi/scan
```

```json
{
  "networks": [
    {"ssid": "MyNetwork", "rssi": -48, "auth": 3}
  ]
}
```

`auth` is the numeric ESP-IDF `wifi_auth_mode_t` value (0 = open, 3 = WPA2-PSK). When the scan itself fails, `networks` is empty and an `error` field carries the ESP-IDF error name.

## POST /api/wifi/connect

Saves credentials and reboots to join the network. The `{"ok": true}` reply is sent before the reboot. In AP setup mode this endpoint is keyless (you could not have entered a key yet); otherwise the normal auth applies.

| Field | Type | Meaning |
|---|---|---|
| `ssid` | string | Required, non-empty |
| `password` | string | Optional; omit or empty for an open network |

```bash
curl -X POST http://192.168.1.50/api/wifi/connect \
  -d '{"ssid": "MyNetwork", "password": "hunter2"}'
```

Errors: 400 `"ssid required"`.

## POST /api/wifi/reset

Clears the stored credentials, replies `{"ok": true}` and reboots into the setup AP. No body needed. Same AP-mode auth exception as connect.

## POST /api/ota/upload

Flashes a firmware image streamed as the raw request body: no JSON, no multipart.

```bash
curl -X POST http://192.168.1.50/api/ota/upload \
  -H "X-Api-Key: mysecret" \
  --data-binary @led-display.bin
```

On success the image is validated, the boot partition switched and the device reboots shortly after replying:

```json
{"ok": true, "note": "update applied, rebooting"}
```

Errors:

| Status | Message | Cause |
|---|---|---|
| 400 | `empty body: POST the raw .bin image` | No body |
| 409 | `another update is already in progress` | A second upload or URL pull is running |
| 413 | `image larger than the OTA slot` | Image does not fit the inactive partition |
| 500 | `receive failed at byte N of M` / `flash write failed ...` | Transfer or flash error; the update is aborted |
| 400 | the `last_result` text from `/api/ota/status` | The transfer completed but the payload was not a bootable image |

Rollback is armed: if the new image crashes before the web server comes up, the next boot reverts to the previous slot.

## POST /api/ota/url

Hands the update to an async downloader and returns immediately; poll `/api/ota/status` for progress. Both `http://` and `https://` URLs work (HTTPS uses the ESP-IDF certificate bundle).

```bash
curl -X POST http://192.168.1.50/api/ota/url \
  -H "X-Api-Key: mysecret" \
  -d '{"url": "https://example.com/led-display.bin"}'
```

```json
{"ok": true, "note": "update started"}
```

Errors: 400 for a missing `url`, a scheme other than http/https or a URL over the length limit; 409 when an update is already running; 500 when the worker could not start. The outcome also fires the `ota_result` [webhook](../integrations/#outbound-webhooks).

## GET /api/ota/check

Open. Queries the latest GitHub release of the repository configured at build time (`CONFIG_LEDDISP_GITHUB_REPO`) and compares it with the running version. Blocking for one HTTPS round trip, up to 10 seconds on a bad network.

```json
{
  "ok": true,
  "current": "2.1.0",
  "latest": "2.2.0",
  "update_available": true,
  "asset_url": "https://github.com/giovi321/led-display/releases/download/v2.2.0/led_display.bin"
}
```

On failure `ok` is false and `error` explains: GitHub unreachable, repository private, no release, or the release has no `led_display.bin` asset. Installing the offered asset is a normal `POST /api/ota/url` with `asset_url`.

## GET /api/ota/status

Open endpoint, safe to poll.

```bash
curl http://192.168.1.50/api/ota/status
```

```json
{
  "running_version": "2.0.0",
  "running_slot": "ota_0",
  "last_result": "ok: 2.0.0 flashed to ota_1",
  "in_progress": false,
  "progress_pct": 100
}
```

`progress_pct` is 0..100, or -1 while downloading from a server that did not send a content length. `last_result` is a human-readable outcome of the most recent attempt.

## GET /api/config/export

Downloads every persisted setting as one JSON document, served with `Content-Disposition: attachment; filename=led-display-config.json`. WiFi credentials are deliberately excluded so the file can move between devices and people without carrying the network password. It does contain the API key and the WireGuard private key, which is why this GET is auth-gated.

```bash
curl -H "X-Api-Key: mysecret" \
  http://192.168.1.50/api/config/export -o led-display-config.json
```

Top-level shape:

```json
{
  "v": 1,
  "fw": "2.0.0",
  "note": "wifi credentials not included",
  "display": {"text": "...", "color": {"r":80,"g":32,"b":0}, "brightness": 60,
              "brightness_cap": 128, "scroll": false, "speed": 50, "scale": 1,
              "boot_screen": 1},
  "layout": {"v":1, "panel": {}, "grid": {}, "cells": []},
  "colors": {"v":1, "colors": []},
  "value_config": {"v":1, "prefix": "", "suffix": "", "decimals": -1,
                   "thousands": "", "stale_hours": 0, "stale_action": "none",
                   "stale_text": ""},
  "integrations": {"v":1, "api_key": "...", "webhook_url": "",
                   "webhook_events": 0, "device_name": ""},
  "wireguard": {"enabled": false, "private_key": "...", "peer_public_key": "",
                "endpoint_host": "", "endpoint_port": 0, "address": "",
                "allowed_ips": "", "keepalive_s": 0},
  "schedule": {"enabled": false, "on_hour": 8, "on_min": 0, "off_hour": 22,
               "off_min": 0, "days": 127, "utc_offset": 0}
}
```

Two fields use the storage dialect rather than the API dialect: `display.boot_screen` is numeric (0 text, 1 ip, 2 none) and `integrations.webhook_events` is the numeric bitmask, not the name array used by [`/api/integrations`](../integrations/#post-apiintegrations).

## POST /api/config/import

Restores a config document. Each section is applied independently; absent sections are left untouched, so a file containing only `{"schedule": {...}}` is a valid partial restore. The layout section is applied first so an imported text renders onto the imported geometry. A section that fails validation is skipped, not fatal. Body limit: 16384 bytes.

```bash
curl -X POST http://192.168.1.50/api/config/import \
  -H "X-Api-Key: mysecret" -H "Content-Type: application/json" \
  --data-binary @led-display-config.json
```

```json
{
  "ok": true,
  "applied": ["layout", "display", "schedule"],
  "skipped": ["wireguard"]
}
```

Notes on section behavior: numeric fields are clamped to their valid ranges rather than rejected; an empty `wireguard.private_key` keeps the stored key; the `wireguard` section lands in `skipped` when applying the tunnel config fails (including when WireGuard is not compiled in). Errors: 400 `"invalid JSON body"`, 401.
