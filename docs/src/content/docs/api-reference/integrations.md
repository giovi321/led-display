---
title: Integrations, WireGuard and MQTT
description: /api/integrations, /api/wireguard, /api/mqtt, and the outbound webhook payloads, including the status heartbeat
sidebar:
  order: 6
---

## GET /api/integrations

Open. Returns the integration settings. Secrets never appear here: the API key, the web UI password, the webhook secret and the webhook header value are reported only as booleans. The value polling settings have their own endpoint, [`/api/value/poll`](../value-and-colors/#get-apivaluepoll).

```json
{
  "api_key_set": false,
  "webhook_url": "http://192.168.10.5:5678/webhook/led-display",
  "webhook_events": ["boot", "value_stale", "value_recovered"],
  "device_name": "led-display",
  "ui_auth": false,
  "ui_user": "",
  "ui_pass_set": false,
  "webhook_secret_set": true,
  "webhook_hdr_name": "X-Token",
  "webhook_hdr_value_set": false,
  "heartbeat_min": 15
}
```

## POST /api/integrations

Requires `X-Api-Key` when a key is set. Overlay semantics: absent fields keep their current value, an explicitly empty `api_key` or `webhook_url` clears (disables) that feature. The webhook secret and header value follow the rule of the other secrets: an absent or empty field never clears them, only the explicit `clear_*` flag does, so re-saving a form cannot wipe them. A request that carries a clear flag and a new value clears first and then stores the new value.

| Field | Type | Limits |
|-------|------|--------|
| `api_key` | string | max 64 chars, empty clears |
| `webhook_url` | string | max 160 chars, empty disables webhooks |
| `webhook_events` | array of strings | any of `boot`, `ip_change`, `ota_result`, `schedule_on`, `schedule_off`, `value_stale`, `value_recovered`; an unknown name is a 400. `status` is not selectable, the heartbeat sends it |
| `heartbeat_min` | number | 0..1440; 0 turns the status heartbeat off, 400 outside the range. Saving restarts the interval |
| `webhook_secret` | string | max 128 chars, sent as `Authorization: Bearer <secret>` with every webhook; absent or empty keeps the stored one |
| `webhook_hdr_name` | string | max 48 chars, a valid HTTP header name (RFC 7230 token), empty clears the name; otherwise 400 `"webhook_hdr_name must be a header token"` |
| `webhook_hdr_value` | string | max 128 chars, no line breaks (400 `"webhook_hdr_value must not contain line breaks"`); absent or empty keeps the stored one |
| `clear_webhook_secret` | bool | `true` removes the webhook secret |
| `clear_webhook_hdr` | bool | `true` removes the webhook header name and value |
| `device_name` | string | max 32 chars; kept for older clients, see below |
| `ui_auth` | bool | turn the browser login on or off |
| `ui_user` | string | max 32 chars, may be empty |
| `ui_pass` | string | max 64 chars; absent keeps the stored password |

### Web UI login

`ui_auth` puts the whole interface behind HTTP Basic authentication: pages, assets and API alike, not only the endpoints that change something, because a login that leaves the configuration readable is not a login. It is off by default and stays off across updates.

Enabling it with no stored password is a 400 (`"ui_auth needs a ui_pass"`) rather than a silent no-op, so a client that asked for a login and got `{"ok": true}` can trust that it has one. Sending no `ui_pass` keeps whatever is stored, so the username can be changed, or the login toggled, without the password travelling back through the browser.

Once it is on, every request needs the `Authorization: Basic` header, including scripts that already send `X-Api-Key`:

```sh
curl -u admin:secret -H 'X-Api-Key: mysecret' http://<device-ip>/api/status
```

Two deliberate limits. The credentials go over plain HTTP (this device has no TLS server), so the login keeps casual visitors on your network out and does nothing against anyone who can watch the traffic. And it is not applied in [setup-AP mode](../../getting-started/wifi-setup/), where the panel carries the only copy of the setup address and the captive-portal probes have to answer normally; that also makes "forget the WiFi" the way back in if the password is lost.

The device name is no longer webhook-specific: one name is the network hostname, the setup-AP SSID and the webhook `device` field, and it is set in the System tab through [`POST /api/system/device_name`](../system/#post-apisystemdevice_name). This endpoint still accepts `device_name` and writes the same stored value, cleaning it to the hostname charset (letters, digits, hyphens, max 24) instead of rejecting it, so a script written against an earlier firmware keeps working. The GET reports the resolved name, which is `led-display` while none is set.

```sh
curl -X POST http://<device-ip>/api/integrations \
  -H 'Content-Type: application/json' \
  -d '{"webhook_url":"http://192.168.10.5:5678/webhook/led-display","webhook_events":["boot","value_stale","value_recovered"],"heartbeat_min":15}'
```

## Outbound webhook payloads

When an enabled event fires, the device POSTs JSON to `webhook_url` with `Content-Type: application/json`, a 5 second timeout, and one retry after 2 seconds. When a webhook secret or custom header is set, every POST carries them as `Authorization: Bearer <secret>` and `<name>: <value>`. Delivery is fire-and-forget; failures are logged and dropped.

```json
{
  "event": "value_stale",
  "device": "led-display",
  "uptime_s": 86042,
  "data": { "reason": "timeout", "age_s": 14400 }
}
```

`device` is the [device name](../system/#post-apisystemdevice_name), which is how a receiver tells several panels apart. It is `led-display` until you set one, and a new name is used from the next event on.

`data` is present only when the event carries detail:

| Event | Fires | `data` |
|-------|-------|--------|
| `boot` | once the device is fully up (HTTP reachable) | none |
| `ip_change` | station interface got an IP | `{"ip": "192.168.10.8"}` |
| `ota_result` | an update finished, either path | `{"result": "ok", "version": "2.10.0"}` or the error text |
| `schedule_on` | the schedule turned the display on | none |
| `schedule_off` | the schedule turned the display off | none |
| `value_stale` | the [fallback](../../user-guide/value-updates/#fallback) engaged (once per episode) | `{"reason": "timeout", "age_s": 14400}` |
| `value_recovered` | the fallback cleared on the next value | `{"reason": "timeout"}` |
| `status` | the heartbeat interval elapsed, see below | the status object below |

For `value_stale`, `reason` is `timeout`, `poll_failures` or `mqtt_lost`, and `age_s` is the seconds since the last value, -1 when no value has arrived since boot. For `value_recovered`, `reason` is the reason that was active. Having no value since boot is not a reason on its own and sends neither event. The `timeout` and `poll_failures` triggers still fire before the first value, with `age_s` -1. See [what engages the fallback](../../user-guide/value-updates/#what-engages-it).

### Status heartbeat

With `heartbeat_min` above 0 the device posts a `status` event every that many minutes, whichever events are selected. A receiver that stops seeing it knows that the device, its power or its network is gone.

```json
{
  "event": "status",
  "device": "led-display",
  "uptime_s": 86042,
  "data": {
    "ip": "192.168.10.8",
    "rssi": -58,
    "uptime_s": 86042,
    "fw": "2.12.0",
    "brightness": 60,
    "power": "on",
    "source": "integration",
    "text": "USD 4,210.5",
    "value_age_s": 42,
    "fallback": false,
    "fallback_reason": "none"
  }
}
```

| Field | Meaning |
|-------|---------|
| `ip` | the device's WiFi address |
| `rssi` | WiFi signal in dBm |
| `uptime_s` | seconds since boot |
| `fw` | firmware version |
| `brightness` | the stored user brightness, 0..255 |
| `power` | `on` while the panel shows anything, `off` when it is blank |
| `source` | `static` or `integration`, the [content source](../display/#post-apidisplaysource) |
| `text` | what the panel currently shows |
| `value_age_s` | seconds since the last value from any source, -1 when none |
| `fallback` | `true` while the fallback is engaged for `timeout`, `poll_failures` or `mqtt_lost`. It stays `false` before the first value, as in the MQTT state |
| `fallback_reason` | `no_value`, `timeout`, `poll_failures` or `mqtt_lost` while engaged, otherwise `none`. The heartbeat also shows `no_value` for the time between boot and the first value, with `fallback` still `false` |

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
  "status": { "compiled_in": true, "up": false, "held": false, "last_handshake": 0,
              "public_key": "LHQ7g...uDc=", "endpoint_ip": "203.0.113.10",
              "attempts": 1, "stage": 4 }
}
```

`status.public_key` is this device's own public key, derived from the stored private key, the value the peer at the other end has to list. `endpoint_ip`, `attempts` and `stage` are bring-up diagnostics, described in the [WireGuard guide](../../admin-guide/wireguard/#status).

`last_handshake` is unix seconds, 0 until the first handshake. `compiled_in` is false when the firmware was built with `CONFIG_LEDDISP_WIREGUARD` off; every mutating call then returns 501. `held` is true when the [crash-loop guard](../../admin-guide/wireguard/#the-crash-loop-guard) is keeping an enabled tunnel down; re-saving the config lifts it.

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

## GET /api/mqtt

Open. Returns the MQTT configuration and live status; the password is reported only as `password_set`.

```json
{
  "enabled": false,
  "uri": "mqtt://192.168.1.10:1883",
  "username": "",
  "password_set": false,
  "base_topic": "",
  "ha_discovery": false,
  "ha_prefix": "homeassistant",
  "status": { "compiled_in": true, "connected": false }
}
```

`compiled_in` is false when the firmware was built with `CONFIG_LEDDISP_MQTT` off; `POST /api/mqtt` then returns 501.

## POST /api/mqtt

Requires `X-Api-Key` when a key is set. Overlay semantics; an absent or empty `password` keeps the stored one, so re-saving the form never wipes it.

| Field | Type | Notes |
|-------|------|-------|
| `enabled` | bool | applies immediately: connects or tears down; requires a non-empty `uri` |
| `uri` | string | `mqtt://`, `mqtts://` (CA-bundle-verified TLS), `ws://` or `wss://`, max 128 chars |
| `username` | string | max 64 chars; empty connects anonymously |
| `password` | string | max 64 chars; empty keeps the stored one |
| `base_topic` | string | max 64 chars; empty uses the device name |
| `ha_discovery` | bool | publish/withdraw Home Assistant discovery configs |
| `ha_prefix` | string | max 32 chars; empty uses `homeassistant` |

Errors: 400 `"uri required to enable MQTT"`, 400 `"field too long"`, 501 when not compiled in.

The topic layout, the state JSON and the discovery entities are documented in the [MQTT guide](../../admin-guide/mqtt/).
