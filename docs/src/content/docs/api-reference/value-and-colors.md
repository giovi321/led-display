---
title: Value and colors
description: The value endpoints (push, poll, preview), their formatting and fallback config, and the saved color list
sidebar:
  order: 4
---

`/api/value` is the integration workhorse: anything that can POST JSON (a home-automation flow, a cron job, curl) sends a number, the firmware formats it through the value config and puts it on the display. The device can also fetch the value itself from a URL, see [`/api/value/poll`](#get-apivaluepoll). The saved colors are a device-side palette the web UI offers as one-click choices.

| Method | Path | Auth | Purpose |
|---|---|---|---|
| POST | `/api/value` | yes | Show a formatted value |
| POST | `/api/value/preview` | yes | Format a sample value without showing or storing it |
| GET | `/api/value/config` | no | Read the formatting and fallback config |
| POST | `/api/value/config` | yes | Update the config (partial update) |
| GET | `/api/value/poll` | no | Read the URL poll config and status |
| POST | `/api/value/poll` | yes | Update the URL poll config (partial update) |
| POST | `/api/value/poll/now` | yes | Request one poll now |
| GET | `/api/colors` | no | Read the saved color list |
| POST | `/api/colors` | yes | Replace the saved color list |

## POST /api/value

Body: `{"value": <number or string>}`. Numbers are normalized to up to 15 significant digits, so `4.2` and `"4.2"` behave the same. The raw value goes through the [value config](#post-apivalueconfig) and, while the display [content source](../display/#post-apidisplaysource) is `integration`, is shown in the current color, scroll mode and speed. The scroll mode is passed through rather than forced off, so a long formatted value keeps scrolling if that is how the display is set up, and posting a value never rewrites the stored mode or the stored static text. Posting also records the arrival time and clears the [fallback](../../user-guide/value-updates/#fallback).

The formatted value is held in RAM only: after a reboot the panel shows the config's fallback text until the next value arrives. In `static` mode the value is still formatted, remembered and counted as fresh, and the response then carries `"rendered": false`.

```bash
curl -X POST http://192.168.1.50/api/value \
  -H "X-Api-Key: mysecret" -d '{"value": 1234567.891}'
```

Response includes the text that went (or would go) on the display:

```json
{"ok": true, "displayed": "$1,234,567.89", "rendered": true}
```

Errors: 400 `"invalid JSON body"` (bad JSON or body over 512 bytes), 400 `"body must be {\"value\":<string|number>}"` when `value` is missing or another type, 401 on a wrong key.

Non-numeric strings are passed through with prefix and suffix only, so `{"value":"OPEN"}` works as a status display.

## POST /api/value/preview

A dry run of the formatter. It formats `raw` with the stored config overlaid by the optional `cfg` object, which takes the same keys as [`POST /api/value/config`](#post-apivalueconfig) and is not saved. Nothing is displayed or stored, and the fallback is not touched. The web UI preview calls this as you type, so the browser carries no copy of the formatter.

```bash
curl -X POST http://192.168.1.50/api/value/preview \
  -H "X-Api-Key: mysecret" -H "Content-Type: application/json" \
  -d '{"raw": 1234567.891, "cfg": {"prefix": "USD ", "scale": "M", "decimals": 1, "thousands": ","}}'
```

```json
{"text": "USD 1.2M"}
```

`raw` is a number or a string. Errors: 400 `"invalid JSON body"` (bad JSON or body over 2048 bytes), 400 `"body must be {\"raw\":<string|number>,\"cfg\":{...}}"`, and any config error from the table below for a bad `cfg`, such as 400 `"thousands and decimal separator must differ"`.

## GET /api/value/config

```bash
curl http://192.168.1.50/api/value/config
```

```json
{
  "prefix": "$",
  "suffix": "",
  "decimals": 2,
  "thousands": ",",
  "multiplier": 1,
  "scale": "none",
  "scale_letters": "",
  "dec_sep": ".",
  "plus_sign": false,
  "fb_text": "waiting...",
  "fb_mode": "dim",
  "fb_after_min": 360,
  "fb_poll_failures": 3,
  "fb_on_mqtt_loss": false,
  "stale_hours": 6,
  "stale_action": "dim",
  "stale_text": "waiting...",
  "default_text": "waiting..."
}
```

The last four keys are the pre-2.12.0 fields, derived from the fallback settings and kept for one release so older scripts still read something. New clients should ignore them.

## POST /api/value/config

Partial update: only the fields you send change.

| Field | Type | Meaning |
|---|---|---|
| `prefix` | string | Up to 16 characters, prepended to the value |
| `suffix` | string | Up to 16 characters, appended to the value |
| `decimals` | number | -1..6; -1 leaves the value as sent (when there is no multiplier or scale), otherwise round half away from zero to that many decimals |
| `thousands` | string | Single separator character; `""` clears it. A space (`" "`) renders as a thin space, narrower than a typed space, so grouped values stay compact |
| `multiplier` | number | Non-zero, applied before scaling |
| `scale` | string | `none`, `auto`, `k`, `M` or `B` |
| `scale_letters` | string | Up to 23 characters, four space-separated tokens for k, M, B and T. Missing tokens fall back to `k M B T`; empty means the defaults |
| `dec_sep` | string | `.` or `,` |
| `plus_sign` | bool | Prefix positive numbers with `+` |
| `fb_text` | string | Up to 64 characters, shown verbatim while the fallback is engaged; `""` keeps the panel blank |
| `fb_mode` | string | `text`, `marker` or `dim` |
| `fb_after_min` | number | 0..43200 minutes without a value; 0 turns the timeout off |
| `fb_poll_failures` | number | 0..255 consecutive failed polls; 0 turns it off |
| `fb_on_mqtt_loss` | bool | Engage when the MQTT broker has been lost for more than 60 seconds |

The order the formatting fields apply in is described in the [user guide](../../user-guide/value-updates/#formatting). `thousands` and `dec_sep` must not be the same character.

The pre-2.12.0 fields `stale_hours`, `stale_action`, `stale_text` and `default_text` are still accepted for one release and mapped onto the fallback settings. When a request carries both an old field and its `fb_*` replacement, the `fb_*` field wins. See [upgrading from 2.11.0](../../user-guide/value-updates/#upgrading-from-2110) for the mapping.

```bash
curl -X POST http://192.168.1.50/api/value/config \
  -H "X-Api-Key: mysecret" \
  -d '{"prefix":"","suffix":" C","decimals":1,"fb_mode":"marker","fb_after_min":360}'
```

Response: `{"ok": true}`. Errors, each a 400 with the message as text: `"decimals must be -1..6"`, `"multiplier must be a non-zero number"`, `"scale must be none|auto|k|M|B"`, `"dec_sep must be . or ,"`, `"fb_mode must be text|marker|dim"`, `"fb_after_min must be 0..43200"`, `"fb_poll_failures must be 0..255"`, `"thousands and decimal separator must differ"`, and for the old fields `"stale_hours must be 0..65535"` and `"stale_action must be none|dim|marker|text"`. A failed save is 500 `"saving value config failed"`. A changed fallback text or mode shows at once, not at the next tick.

## Fallback

The fallback settings above decide what the panel does when the feed breaks. What triggers it, how it clears, and what it reports are described in one place, the [user guide](../../user-guide/value-updates/#fallback). The reports an API client sees:

- the `value_stale` and `value_recovered` [webhook events](../integrations/#outbound-webhook-payloads), with `reason` one of `timeout`, `poll_failures` or `mqtt_lost`
- the `fallback` and `fallback_reason` fields of the [MQTT state](../../admin-guide/mqtt/)
- the same two fields in the `status` heartbeat, where `fallback_reason` can also be `no_value` before the first value since boot, with `fallback` false
- `value_age_s` in [`GET /api/status`](../system/#get-apistatus): seconds since the last value from any source, -1 when none was ever received

## GET /api/value/poll

Open. Returns the URL poll config and the status of the last poll. The custom header value never appears: it is reported only as a boolean.

```bash
curl http://192.168.1.50/api/value/poll
```

```json
{
  "enabled": true,
  "url": "https://example.com/value.json",
  "interval_s": 300,
  "hdr_name": "X-Token",
  "hdr_value_set": false,
  "path": "data.price",
  "status": {
    "last_poll": 1793011200,
    "http_status": 200,
    "raw": "4210.5",
    "error": "",
    "failures": 0
  }
}
```

`status.last_poll` is unix seconds, 0 until the first poll. `http_status` is 0 when there was no response. `raw` is the extracted value (cut to 63 characters, for display) and `error` is empty after a good poll; after a failed one `raw` is empty and `error` holds the reason, for example `HTTP 503`, `path not found`, `value is not a string or number`, `invalid JSON`, `response too large`, `empty response` or a transport error such as `ESP_ERR_TIMEOUT`. A connection that cannot be opened names the cause: `DNS lookup failed`, `TCP connect failed`, `TCP connect timed out`, `TLS handshake timed out`, `TLS handshake failed (-0x....)` with the mbedTLS error code, or `TLS certificate not trusted (flags 0x..)` with the mbedTLS verification flags. `failures` counts consecutive failed polls.

## POST /api/value/poll

Requires `X-Api-Key` when a key is set. Overlay semantics: absent fields keep their stored value. Saving applies immediately and triggers one poll, so the request itself never waits for the network.

| Field | Type | Meaning |
|---|---|---|
| `enabled` | bool | turn polling on or off; enabling needs a URL |
| `url` | string | `http://` or `https://`, up to 255 characters; `""` clears it |
| `interval_s` | number | 10..86400 seconds |
| `path` | string | up to 64 characters; empty means the whole trimmed response body is the value |
| `hdr_name` | string | up to 48 characters, a valid HTTP header name (RFC 7230 token); `""` clears the name. This is where authentication goes: for a bearer token use `Authorization` |
| `hdr_value` | string | up to 128 characters, no line breaks, for example `Bearer <token>`; absent or empty keeps the stored one |
| `clear_header` | bool | `true` removes the header name and value |

The header value is removed only by `clear_header`, never by an absent or empty field, so re-saving a form cannot wipe it. A request that carries a clear flag and a new value clears first and then stores the new value.

```bash
curl -X POST http://192.168.1.50/api/value/poll \
  -H "X-Api-Key: mysecret" -H "Content-Type: application/json" \
  -d '{"enabled": true, "url": "https://example.com/value.json", "path": "data.price", "interval_s": 300, "hdr_name": "Authorization", "hdr_value": "Bearer <token>"}'
```

Response: `{"ok": true}`. Errors: 400 `"invalid JSON body"`, 400 `"interval_s must be 10..86400"`, 400 `"url must start with http:// or https://"`, 400 `"url required to enable polling"`, 400 `"field too long"`, 400 `"hdr_name must be a header token"`, 400 `"hdr_value must not contain line breaks"`, 400 `"hdr_value requires hdr_name"` when the saved result would have a header value without a name, 500 `"saving value poll config failed"`.

Polling runs only while it is enabled, a URL is set and the device is in station mode with an IP address. Each poll has a 10 second timeout, follows redirects, verifies HTTPS against the built-in certificate bundle, and accepts a response body of up to 4 KB. How a response becomes a value, and what counts as a failure, is in the [user guide](../../user-guide/value-updates/#fetching-the-value-from-a-url).

## POST /api/value/poll/now

Requires `X-Api-Key` when a key is set. Asks the poller to run one poll now and returns at once with the current status. It does not wait for the result.

```bash
curl -X POST http://192.168.1.50/api/value/poll/now -H "X-Api-Key: mysecret"
```

```json
{"ok": true, "pending": true, "status": {"last_poll": 1793011200, "http_status": 200, "raw": "4210.5", "error": "", "failures": 0}}
```

`pending` is `true` when a poll was requested. It is `false`, and nothing is triggered, when polling cannot run: it is disabled, no URL is set, or the device has no station IP. The `status` block is the one from before the new poll. Read [`GET /api/value/poll`](#get-apivaluepoll) until `last_poll` changes; the web UI gives up after about 12 seconds.

## GET /api/colors

```bash
curl http://192.168.1.50/api/colors
```

```json
{
  "colors": [
    {"name": "amber", "r": 80, "g": 32, "b": 0},
    {"name": "mint", "r": 0, "g": 96, "b": 40}
  ]
}
```

An empty list is returned when nothing was saved yet.

## POST /api/colors

Replaces the whole list; there is no per-entry add or delete. At most 12 entries, each with a non-empty `name` of up to 24 characters and `r`, `g`, `b` in 0..255. Extra fields on an entry are dropped.

```bash
curl -X POST http://192.168.1.50/api/colors \
  -H "X-Api-Key: mysecret" -H "Content-Type: application/json" \
  -d '{"colors":[{"name":"amber","r":80,"g":32,"b":0}]}'
```

Response: `{"ok": true}`. Errors: 400 `"colors must be <=12 entries of {name<=24ch,r,g,b 0..255}"` when any entry fails validation, 500 `"saving colors failed"`.
