---
title: API conventions
description: Base URL, JSON envelope, authentication and partial-update semantics shared by every endpoint
sidebar:
  order: 1
---

The firmware exposes a plain HTTP JSON API on port 80. Everything the web UI does goes through these endpoints, so anything you can click you can also script. This page covers the rules shared by all endpoints; the per-endpoint pages are:

- [Display control](display/): text, fonts, brightness, the light sensor, test patterns, identify
- [Layout](layout/): the panel arrangement config
- [Value and colors](value-and-colors/): the generic value endpoint, its formatting config, saved colors
- [System](system/): status, reboot, device name, schedule, WiFi, OTA, config export/import
- [Integrations](integrations/): API key, outbound webhooks, WireGuard

## Base URL

All paths are relative to `http://<device-ip>`. There is no TLS; the API is designed for a trusted LAN or a WireGuard tunnel (see [integrations](integrations/#get-apiwireguard)).

```bash
curl http://192.168.1.50/api/status
```

## JSON everywhere

Request and response bodies are JSON with `Content-Type: application/json`, with two exceptions:

- `POST /api/ota/upload` takes the raw firmware binary as the request body
- `GET /api/config/export` returns JSON but adds a `Content-Disposition: attachment` header so browsers save it as a file

Responses carry `Cache-Control: no-store`.

## The ok/error envelope

A successful mutation returns:

```json
{"ok": true}
```

Some endpoints add fields next to `ok` (for example `displayed` on `POST /api/value`, or `applied`/`skipped` on config import). GET endpoints return their resource object directly, without an `ok` wrapper.

Every error, whatever the status code, has one shape:

```json
{"ok": false, "error": "human-readable reason"}
```

Status codes used: 400 (bad or missing input), 401 (auth), 409 (an OTA update is already running), 413 (OTA image larger than the slot), 500 (internal failure, for example NVS write or driver reconfigure), 501 (WireGuard requested but not compiled in).

A body that is not valid JSON, or larger than the endpoint's size limit, yields a 400 with `"bad json"` or `"invalid JSON body"`. Limits are generous for the intended payloads: 4096 bytes for `/api/layout`, 16384 bytes for `/api/config/import`, 512 bytes for `/api/value`, 1024 to 2048 bytes elsewhere.

## Authentication

Out of the box there is no key and every endpoint is open (LAN-trust default). Once you set an API key via `POST /api/integrations`, clients must send it in the `X-Api-Key` header:

```bash
curl -X POST http://192.168.1.50/api/display/clear \
  -H "X-Api-Key: mysecret"
```

What the key gates:

| Scope | Key required |
|---|---|
| All POST endpoints | yes |
| `GET /api/config/export` | yes (the export contains the API key and the WireGuard private key) |
| All other GET endpoints | no |
| `/api/wifi/*` on the setup AP of a device with no saved network | no (first-time provisioning: no key could have been set yet) |
| `/api/wifi/*` on the fallback setup AP of a provisioned device | yes (that AP is open, so the exception would hand it to anyone in range) |

A missing or wrong key returns:

```
HTTP/1.1 401 Error
{"ok": false, "error": "missing or wrong X-Api-Key"}
```

The key is compared in constant time, is at most 64 characters, and setting it to an empty string disables auth again. GET endpoints that touch secrets never echo them: they report `api_key_set` / `private_key_set` booleans instead.

## Where the request claims to be going

Every request that goes through the auth check (all POSTs, plus `GET /api/config/export`) is also checked against the address it was sent to. The `Host` header, and the `Origin` header when the client sends one, must name:

- the device's WiFi IP address
- its WireGuard address, when a tunnel is configured
- its device name, optionally followed by one DNS suffix label (`led-display.local`, `office-panel.lan`)

A port after the name or address is fine. Anything else returns:

```
HTTP/1.1 403 Error
{"ok": false, "error": "cross-origin request rejected"}
```

This is a DNS-rebinding and CSRF guard, which matters because the default configuration has no key at all: without it, any page a browser on your LAN loads could post to the display. Scripts and `curl` are unaffected as long as they address the device by IP or by name. The check is skipped while the device runs its own setup AP, where the captive portal answers under whatever hostname the client probed.

## Partial updates

Config-style POST endpoints overlay the fields you send onto the current state; anything you omit keeps its value. This applies to:

- `POST /api/display/text` (omitted text/color/scroll_mode/speed/font/spacing/scale are preserved)
- `POST /api/display/light_sensor`
- `POST /api/schedule`
- `POST /api/value/config`
- `POST /api/integrations`
- `POST /api/wireguard` (an absent or empty `private_key` keeps the stored one)

Two endpoints replace the whole resource instead, so the client never has to diff: `POST /api/colors` (the full list) and `POST /api/layout` (the full arrangement).

## Firmware version

This reference matches firmware 2.2.0, reported in `GET /api/status` under `system.fw`. Fields added in 2.2.0 are optional everywhere: a client that only knows the earlier shapes keeps working, and the legacy `scroll` bool is still accepted alongside `scroll_mode`. The v1 map-mode API survives as a compatibility shim at [`POST /api/display/map`](display/#post-apidisplaymap).
