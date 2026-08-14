---
title: Integrations and WireGuard
description: /api/integrations, /api/wireguard, and the outbound webhook payloads
sidebar:
  order: 6
---

## GET /api/integrations

Open. Returns the integration settings. Secrets never appear here: the API key is reported only as a boolean.

```json
{
  "api_key_set": false,
  "webhook_url": "http://192.168.10.5:5678/webhook/led-display",
  "webhook_events": ["boot", "value_stale"],
  "device_name": "led-display"
}
```

## POST /api/integrations

Requires `X-Api-Key` when a key is set. Overlay semantics: absent fields keep their current value, an explicitly empty `api_key` or `webhook_url` clears (disables) that feature.

| Field | Type | Limits |
|-------|------|--------|
| `api_key` | string | max 64 chars, empty clears |
| `webhook_url` | string | max 160 chars, empty disables webhooks |
| `webhook_events` | array of strings | any of `boot`, `ip_change`, `ota_result`, `schedule_on`, `schedule_off`, `value_stale`; an unknown name is a 400 |
| `device_name` | string | max 32 chars, reported in webhook payloads |

```sh
curl -X POST http://<device-ip>/api/integrations \
  -H 'Content-Type: application/json' \
  -d '{"webhook_url":"http://192.168.10.5:5678/webhook/led-display","webhook_events":["boot","value_stale"]}'
```

## Outbound webhook payloads

When an enabled event fires, the device POSTs JSON to `webhook_url` with `Content-Type: application/json`, a 5 second timeout, and one retry after 2 seconds. Delivery is fire-and-forget; failures are logged and dropped.

```json
{
  "event": "value_stale",
  "device": "led-display",
  "uptime_s": 86042,
  "data": { "age_s": 14400 }
}
```

`data` is present only when the event carries detail:

| Event | Fires | `data` |
|-------|-------|--------|
| `boot` | once the device is fully up (HTTP reachable) | none |
| `ip_change` | station interface got an IP | `{"ip": "192.168.10.8"}` |
| `ota_result` | an update finished, either path | `{"result": "ok", "version": "2.1.0"}` or the error text |
| `schedule_on` | the schedule turned the display on | none |
| `schedule_off` | the schedule turned the display off | none |
| `value_stale` | the staleness guard tripped (once per episode) | `{"age_s": <seconds since last value>}` |

## GET /api/wireguard

Open. Returns the tunnel configuration and live status; the private key is reported only as `private_key_set`.

```json
{
  "enabled": false,
  "private_key_set": false,
  "peer_public_key": "",
  "endpoint_host": "",
  "endpoint_port": 51820,
  "address": "",
  "allowed_ips": "",
  "keepalive_s": 25,
  "status": { "compiled_in": true, "up": false, "last_handshake": 0 }
}
```

`last_handshake` is unix seconds, 0 until the first handshake. `compiled_in` is false when the firmware was built with `CONFIG_LEDDISP_WIREGUARD` off; every mutating call then returns 501.

## POST /api/wireguard

Requires `X-Api-Key` when a key is set. Overlay semantics like `/api/integrations`; an absent or empty `private_key` keeps the stored one, so re-saving the form never wipes the key.

| Field | Type | Notes |
|-------|------|-------|
| `enabled` | bool | applies immediately: connects or tears down |
| `private_key` | string | this device's key, base64 |
| `peer_public_key` | string | the server's public key |
| `endpoint_host` | string | hostname or IP of the peer |
| `endpoint_port` | number | 0..65535, typically 51820 |
| `address` | string | tunnel address with mask, e.g. `10.6.0.12/32` |
| `allowed_ips` | string | comma-separated CIDRs routed into the tunnel |
| `keepalive_s` | number | persistent keepalive, 0 disables, 25 is typical |

See the [WireGuard guide](../../admin-guide/wireguard/) for setup and limitations.
