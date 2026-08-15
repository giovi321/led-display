---
title: System
description: Status, reboot, boot screen, factory reset, schedule, WiFi, OTA updates and config export/import
sidebar:
  order: 5
---

Everything that keeps the device running: the status tree the web UI polls, power-cycle style operations, the on/off schedule, WiFi provisioning, firmware updates and whole-config backup. Auth follows the [shared rules](../): POSTs need `X-Api-Key` when a key is set, GETs are open except `/api/config/export`, and every checked request must also address the device by its own [IP, WireGuard address or name](../#where-the-request-claims-to-be-going) or it is refused with 403 `"cross-origin request rejected"`.

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/api/status` | no | Full device status |
| POST | `/api/system/reboot` | yes | Reboot |
| POST | `/api/system/boot_screen` | yes | What to show right after boot |
| POST | `/api/system/screens` | yes | The startup splash and the WiFi-lost screen |
| POST | `/api/system/device_name` | yes | Set the one name used for hostname, setup AP and webhooks |
| POST | `/api/system/factory_reset` | yes | Wipe settings, keep WiFi |
| GET | `/api/schedule` | no | Read the on/off schedule |
| POST | `/api/schedule` | yes | Update the schedule (partial update) |
| GET | `/api/wifi/scan` | no | Scan for networks |
| GET | `/api/wifi/networks` | no | List the saved networks |
| POST | `/api/wifi/networks` | yes, except when provisioning | Add or update a saved network |
| POST | `/api/wifi/networks/delete` | yes, except when provisioning | Forget one saved network |
| POST | `/api/wifi/connect` | yes, except when provisioning | Save a network and reboot onto it |
| POST | `/api/wifi/reset` | yes, except when provisioning | Forget every network and reboot into setup |
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
    "connected": true,
    "ap_ssid": "LEDDisplay-Setup"
  },
  "display": {
    "text": "$1,234,567.89",
    "scroll": false,
    "scroll_mode": "off",
    "speed": 50,
    "scale": 1,
    "font": "5x7",
    "spacing": "proportional",
    "brightness": 60,
    "brightness_stored": 60,
    "text_stored": "HELLO",
    "source": "static",
    "color": {"r": 80, "g": 32, "b": 0}
  },
  "system": {
    "fw": "2.2.0",
    "chip": "ESP32-S3",
    "cores": 2,
    "rev": 3,
    "heap": 123456,
    "uptime": 3600,
    "brightness_cap": 128,
    "device_name": "led-display",
    "device_name_stored": "",
    "boot_screen": "ip",
    "startup_screen": "border",
    "offline_screen": "keep",
    "offline_text": "",
    "offline_x": 0,
    "offline_y": 0,
    "offline_brightness": 255
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
    "held": false,
    "last_handshake": 0
  },
  "mqtt": {
    "compiled_in": true,
    "enabled": false,
    "connected": false
  },
  "ota": {
    "slot": "ota_0",
    "version": "2.2.0",
    "in_progress": false
  },
  "light_sensor": {
    "enabled": false,
    "sampling": false,
    "raw": -1,
    "level": -1
  },
  "schedule_active": true
}
```

| Field | Meaning |
|---|---|
| `wifi.mode` | `sta` (joined a network), `ap` (setup portal) or `boot` (still deciding) |
| `wifi.rssi` | Signal strength in dBm |
| `wifi.ap_ssid` | The SSID the setup network appears under, reported in every mode |
| `display.*` | Current text state; `scroll_mode`, `font`, `spacing`, `speed` and `scale` as documented under [display control](../display/#post-apidisplaytext) |
| `display.scroll` | The legacy bool, true for both scrolling modes |
| `display.brightness` | The level the panel is actually running at: 0 in a blanked off-window, the light sensor's level while it drives, halved when the value is stale, never above the cap |
| `display.brightness_stored` | The brightness you set and the device persisted. It is what the display returns to when the schedule, the sensor or the staleness guard stops overriding it |
| `display.text_stored` | The persisted static text, distinct from `display.text` (whatever is on screen - the pushed value in integration mode) |
| `display.source` | `static` or `integration`, see [`POST /api/display/source`](../display/#post-apidisplaysource) |
| `system.heap` | Free heap in bytes |
| `system.uptime` | Seconds since boot |
| `system.device_name` | The resolved name, so a client never has to know the default |
| `system.device_name_stored` | The name you set, empty when none is stored |
| `system.boot_screen` | What settles on the panel after boot: `text` (the configured content), `ip` or `none` |
| `system.startup_screen` | The splash before the network is up: `border`, `blank` or `content` |
| `system.offline_screen` | What a lost WiFi link puts on the panel: `keep`, `text`, `dot` or `blank` |
| `system.offline_text` | The message shown when `offline_screen` is `text` |
| `system.offline_x`, `system.offline_y` | Where the `dot` marker sits |
| `system.offline_brightness` | The marker's own brightness, 0..255 |
| `layout.*` | Summary of the active layout; the full config comes from [`GET /api/layout`](../layout/) |
| `value_age_s` | Seconds since the last `POST /api/value`; -1 means never |
| `wireguard.last_handshake` | Unix seconds, 0 means never |
| `wireguard.held` | The [crash-loop guard](../../admin-guide/wireguard/#the-crash-loop-guard) is keeping an enabled tunnel down |
| `mqtt.*` | MQTT client state; config via [`GET /api/mqtt`](../integrations/#get-apimqtt) |
| `ota.slot` | Running partition: `factory`, `ota_0` or `ota_1` |
| `light_sensor` | Ambient sensor state; the full config comes from [`GET /api/display/light_sensor`](../display/#get-apidisplaylight_sensor) |
| `schedule_active` | `true` when the schedule currently allows the display to be on |

## POST /api/system/reboot

Replies `{"ok": true}`, then reboots 500 ms later so the response gets out. No body needed.

```bash
curl -X POST http://192.168.1.50/api/system/reboot -H "X-Api-Key: mysecret"
```

## POST /api/system/boot_screen

Chooses what settles on the display once the network is up. The two screens either side of this one - the startup splash and the WiFi-lost screen - are set through `/api/system/screens` below.

| Field | Type | Meaning |
|---|---|---|
| `mode` | string | `text` (the configured content, static or integration), `ip_then_content` (the address for five seconds, then the content), `ip` or `none` |

```bash
curl -X POST http://192.168.1.50/api/system/boot_screen \
  -H "X-Api-Key: mysecret" -d '{"mode": "ip"}'
```

Errors: 400 `"unknown mode"`, 500 `"saving boot screen failed"` when the write to flash fails.

## POST /api/system/screens

The startup splash and what a lost WiFi link puts on the panel. Partial update: absent fields keep their stored value.

| Field | Type | Meaning |
|---|---|---|
| `startup_screen` | string | `border` (1 s border test, the default), `blank`, or `content` to skip the splash entirely |
| `offline_screen` | string | `keep` (leave the last content, the default), `text`, `dot`, or `blank` |
| `offline_text` | string | Up to 63 characters, shown when `offline_screen` is `text` |
| `offline_color` | object | `{"r":0..255,"g":0..255,"b":0..255}`, the colour of the `dot` pixel |
| `offline_x` | number | Marker column, 0 to `layout.width` − 1 |
| `offline_y` | number | Marker row, 0 to `layout.height` − 1 |
| `offline_brightness` | number | 0..255, the marker's own brightness |

The border default is deliberate: it lights every edge pixel, so a miswired or half-dead panel announces itself in the first second after power-on, before anything else can mask it.

`offline_screen` applies when the station link drops while the device keeps running. It does **not** apply in [setup-AP mode](../../getting-started/wifi-setup/), where the panel carries the only copy of the setup SSID and address and that banner always wins.

```bash
curl -X POST http://192.168.1.50/api/system/screens \
  -H "X-Api-Key: mysecret" -H "Content-Type: application/json" \
  -d '{"startup_screen":"border","offline_screen":"text","offline_text":"OFFLINE"}'
```

`dot` is the only one of the four that is an overlay rather than a screen. The panel goes on showing whatever it was showing, and one pixel at (`offline_x`, `offline_y`) is lit on top of it: the quietest way for a wall-mounted display to admit it has lost the network without also throwing away the number someone is standing there reading. `offline_brightness` is the marker's own, applied instead of the panel brightness and its cap, so the pixel stays visible on a panel the light sensor has dimmed or the schedule has turned off. Coming back online removes the pixel and repaints nothing.

`offline_x` and `offline_y` are validated against the configured panel, so the bounds move with [the layout](../layout/). Before 2.7.0 the marker was pinned to a corner with `offline_corner`; a stored corner is converted to coordinates on the first boot of 2.7.0 and the field is gone from the API.

Errors: 400 `"startup_screen must be border|blank|content"`, 400 `"offline_screen must be keep|text|blank|dot"`, 400 `"offline_text too long (max 63)"`, 400 `"offline_x must be 0..N"`, 400 `"offline_y must be 0..N"`, 500 `"saving screens failed"`.

## POST /api/system/device_name

Sets the one name that identifies the device in three places: the network hostname on both interfaces, the SSID of the setup network, and the `device` field of the [outbound webhooks](../integrations/#outbound-webhook-payloads).

| Field | Type | Meaning |
|---|---|---|
| `name` | string | Required. 1 to 24 characters, letters, digits and hyphens, starting and ending with a letter or digit. An empty string clears the name |

```bash
curl -X POST http://192.168.1.50/api/system/device_name \
  -H "X-Api-Key: mysecret" -d '{"name": "office-panel"}'
```

```json
{"ok": true, "device_name": "office-panel", "hostname": "office-panel",
 "ap_ssid": "office-panel"}
```

The response reports what the name now resolves to, so a client does not have to duplicate the default rules. With no name stored, the hostname and the webhook `device` field are `led-display` and the setup network is `LEDDisplay-Setup`, exactly as before this endpoint existed.

Webhooks use a new name immediately. The hostname follows on the next DHCP renewal or reboot, and the setup network is renamed the next time it comes up, so saving a name never disconnects whoever is on the portal. A device name set through `POST /api/integrations` by an earlier build is adopted here once, cleaned to the charset above.

Errors: 400 `"name required"` when the field is missing or not a string, 400 `"name must be 1-24 letters, digits or hyphens, starting and ending with a letter or digit"`, 500 `"saving device name failed"`.

## POST /api/system/factory_reset

Erases the settings namespace (text, font and spacing, brightness and cap, layout, colors, value config, integrations, device name, WireGuard, light sensor) and reboots. WiFi credentials live in a separate NVS namespace and survive on purpose, so a factory reset cannot strand a wall-mounted device off the network; use `/api/wifi/reset` for that. The schedule has its own namespace as well and is left as it is.

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
  "utc_offset": 60,
  "tz": "Europe/Zurich",
  "tz_known": true,
  "tz_posix": "<+01>-1<+02>-2,M3.5.0,M10.5.0/3",
  "off_action": "off",
  "off_brightness": 16
}
```

| Field | Meaning |
|---|---|
| `days` | Bitmask, bit 0 = Monday through bit 6 = Sunday; 127 = every day, 31 = weekdays |
| `utc_offset` | Minutes from UTC, for example 60 for CET. Used only while `tz` is empty |
| `tz` | IANA zone name, e.g. `Europe/Zurich`. When set, the schedule follows that zone's rule (daylight-saving transitions included) and `utc_offset` is ignored. Empty = fixed offset |
| `tz_known` | Whether the device recognises `tz`. False means it is running on UTC instead - a typo, or a zone newer than the firmware's table |
| `tz_posix` | The POSIX rule `tz` resolved to, empty when unknown. Diagnostic only |
| `on_hour`/`on_min`, `off_hour`/`off_min` | Local switch-on and switch-off times; a window that wraps midnight (on 22:00, off 06:00) works |
| `off_action` | What the off window does: `off` blanks the panel (the default), `dim` holds it at `off_brightness` |
| `off_brightness` | The level the `dim` action holds, 1..255. Still subject to the brightness cap |

## POST /api/schedule

Partial update: only fields present in the body change, so a client that does not know `off_action` cannot change it by accident.

```bash
curl -X POST http://192.168.1.50/api/schedule \
  -H "X-Api-Key: mysecret" \
  -d '{"enabled":true,"on_hour":8,"off_hour":22,"days":127,"utc_offset":60,
       "off_action":"dim","off_brightness":16}'
```

Out-of-range values are rejected rather than wrapped: hours must be 0..23, minutes 0..59, `utc_offset` -840..840 and `off_brightness` 1..255. `days` is masked to its seven bits. `tz` accepts up to 48 printable ASCII characters; an empty string clears the override. A name the device's table does not carry is stored and passed to libc as-is, which is what makes a raw POSIX TZ string work too; check `tz_known` on the GET to tell the two apart.

Response: `{"ok": true}`. A changed action or level is applied at once, not at the next 30 second tick. Schedule transitions fire the `schedule_on` / `schedule_off` [webhooks](../integrations/#outbound-webhook-payloads) whichever action is configured.

Errors: 400 `"hours 0-23, minutes 0-59, utc_offset -840..840"`, 400 `"off_brightness 1-255"`, 400 `"off_action must be off or dim"`, 500 `"schedule applied but not saved"`.

The 500 is worth handling rather than retrying blindly: the schedule is applied before it is written, so a failed write leaves the new schedule running until the next reboot, after which the device is back on the previously stored one.

## GET /api/wifi/scan

Returns up to 20 visible networks.

```bash
curl http://192.168.1.50/api/wifi/scan
```

```json
{
  "networks": [
    {"ssid": "MyNetwork", "rssi": -48, "auth": 3, "saved": true}
  ]
}
```

`auth` is the numeric ESP-IDF `wifi_auth_mode_t` value (0 = open, 3 = WPA2-PSK). `saved` marks the networks the device already has credentials for, so a client can offer them without asking for a password again. When the scan itself fails, `networks` is empty and an `error` field carries the ESP-IDF error name.

## GET /api/wifi/networks

Open. Lists the saved networks. Passwords are write-only: no endpoint ever returns one.

```bash
curl http://192.168.1.50/api/wifi/networks
```

```json
{
  "networks": [
    {"ssid": "MyNetwork", "connected": true},
    {"ssid": "Hotspot", "connected": false}
  ],
  "count": 2,
  "max": 5
}
```

`connected` marks the network the station interface is currently on. `max` is the number of slots the firmware keeps, five.

## POST /api/wifi/networks

Adds a network, or replaces the password of one that is already stored. Nothing reboots. An online device keeps the connection it has, which is how a fallback network gets added while the display is in use; a device that is offline or sitting on its setup AP tries the new network within moments and takes the AP down if it joins.

"Except when provisioning" means the one keyless case: the setup AP of a device that has no saved network yet. A provisioned device on its fallback AP needs the key like everywhere else, see the [auth rules](../#authentication).

| Field | Type | Meaning |
|---|---|---|
| `ssid` | string | Required, non-empty, max 32 characters |
| `password` | string | Optional, max 64 characters; omit or empty for an open network |

```bash
curl -X POST http://192.168.1.50/api/wifi/networks \
  -H "X-Api-Key: mysecret" -d '{"ssid": "Hotspot", "password": "hunter2"}'
```

```json
{"ok": true, "count": 2}
```

Errors: 400 `"ssid required"`, 400 `"ssid max 32 chars, password max 64"`, 409 `"all network slots are in use"` when all five are taken (forget one first), 500 `"could not save"`.

## POST /api/wifi/networks/delete

Forgets one saved network. The connection is not dropped, so removing the network the device is currently on takes effect at the next boot.

```bash
curl -X POST http://192.168.1.50/api/wifi/networks/delete \
  -H "X-Api-Key: mysecret" -d '{"ssid": "Hotspot"}'
```

Response: `{"ok": true, "count": 1}`. Errors: 400 `"ssid required"`, 404 `"not saved"`, 500 `"could not save"`.

## POST /api/wifi/connect

Adds or updates a network like `POST /api/wifi/networks`, then reboots so the device comes up on it from a clean start. The other saved networks stay. The `{"ok": true}` reply is sent before the reboot. Same provisioning exception as above.

| Field | Type | Meaning |
|---|---|---|
| `ssid` | string | Required, non-empty, max 32 characters |
| `password` | string | Optional, max 64 characters; omit or empty for an open network |

```bash
curl -X POST http://192.168.1.50/api/wifi/connect \
  -d '{"ssid": "MyNetwork", "password": "hunter2"}'
```

Errors: 400 `"ssid required"`, 400 `"ssid max 32 chars, password max 64"`.

## POST /api/wifi/reset

Forgets every saved network, replies `{"ok": true}` and reboots into the setup AP. No body needed. Same provisioning exception as connect, which in practice means a device with a key set needs it here: the endpoint is only keyless while nothing is saved, and then there is nothing to reset.

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

Errors: 400 for a missing `url`, a scheme other than http/https or a URL over the length limit; 409 when an update is already running; 500 when the worker could not start. The outcome also fires the `ota_result` [webhook](../integrations/#outbound-webhook-payloads).

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
  "running_version": "2.2.0",
  "running_slot": "ota_0",
  "last_result": "ok: 2.2.0 flashed to ota_1",
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
  "fw": "2.3.0",
  "note": "wifi credentials not included",
  "display": {"text": "...", "color": {"r":80,"g":32,"b":0}, "brightness": 60,
              "brightness_cap": 128, "scroll": false, "scroll_mode": "off",
              "speed": 50, "scale": 1, "font": "5x7",
              "spacing": "proportional", "boot_screen": 1, "source": "static"},
  "layout": {"v":1, "panel": {}, "grid": {}, "cells": []},
  "colors": {"v":1, "colors": []},
  "value_config": {"v":1, "prefix": "", "suffix": "", "decimals": -1,
                   "thousands": "", "stale_hours": 0, "stale_action": "none",
                   "stale_text": "", "default_text": ""},
  "integrations": {"v":1, "api_key": "...", "webhook_url": "",
                   "webhook_events": 0, "device_name": "led-display"},
  "wireguard": {"enabled": false, "private_key": "...", "peer_public_key": "",
                "endpoint_host": "", "endpoint_port": 0, "address": "",
                "allowed_ips": "", "keepalive_s": 0},
  "mqtt": {"v":1, "enabled": false, "uri": "", "username": "", "password": "...",
           "base_topic": "", "ha_discovery": false, "ha_prefix": "homeassistant"},
  "schedule": {"enabled": false, "on_hour": 8, "on_min": 0, "off_hour": 22,
               "off_min": 0, "days": 127, "utc_offset": 0, "tz": "",
               "off_action": "off", "off_brightness": 16},
  "light_sensor": {"v":1, "enabled": false, "gpio": 0, "dark_raw": 200,
                   "bright_raw": 2800, "min_brightness": 8,
                   "max_brightness": 128, "gamma_x10": 10}
}
```

Two fields use the storage dialect rather than the API dialect: `display.boot_screen` is numeric (0 text, 1 ip, 2 none) and `integrations.webhook_events` is the numeric bitmask, not the name array used by [`/api/integrations`](../integrations/#post-apiintegrations).

The `light_sensor` section describes the configuration only; the live reading is not part of a backup. A file written by an older build simply lacks the sections and fields added since (2.2.0: `light_sensor`, `scroll_mode`, `font`, `spacing`, `off_action`, `off_brightness`; 2.3.0: `mqtt`, `display.source`, `value_config.default_text`, `schedule.tz`); it still imports, and the missing settings keep their current values.

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

Notes on section behavior: numeric fields are clamped to their valid ranges rather than rejected; an empty `wireguard.private_key` (and an empty `mqtt.password`) keeps the stored secret; the `wireguard` and `mqtt` sections land in `skipped` when applying them fails (including when the feature is not compiled in); a `light_sensor` section describing a sensor this board does not have (no pin, or two equal calibration readings) also lands in `skipped` rather than being half-applied. An `integrations.device_name` that predates the [name rules](#post-apisystemdevice_name) is cleaned to them, so `"Office Panel"` imports as `Office-Panel` instead of failing the section. Errors: 400 `"invalid JSON body"`, 401.
