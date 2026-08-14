---
title: Value and colors
description: The generic value endpoint, its formatting and staleness config, and the saved color list
sidebar:
  order: 4
---

`/api/value` is the integration workhorse: anything that can POST JSON (a home-automation flow, a cron job, curl) sends a number, the firmware formats it through the value config and puts it on the display. The saved colors are a device-side palette the web UI offers as one-click choices.

| Method | Path | Auth | Purpose |
|---|---|---|---|
| POST | `/api/value` | yes | Show a formatted value |
| GET | `/api/value/config` | no | Read the formatting and staleness config |
| POST | `/api/value/config` | yes | Update the config (partial update) |
| GET | `/api/colors` | no | Read the saved color list |
| POST | `/api/colors` | yes | Replace the saved color list |

## POST /api/value

Body: `{"value": <number or string>}`. Numbers are normalized to up to 10 significant digits, so `4.2` and `"4.2"` behave the same. The raw value goes through the value config (prefix, suffix, decimals, thousands separator) and, while the display [content source](../display/#post-apidisplaysource) is `integration`, is shown in the current color, scroll mode and speed. The scroll mode is passed through rather than forced off, so a long formatted value keeps scrolling if that is how the display is set up, and posting a value never rewrites the stored mode or the stored static text. Posting also resets the staleness clock (see below).

The formatted value is held in RAM only: after a reboot the panel shows the config's `default_text` placeholder until the next POST. In `static` mode the value is still formatted, remembered and counted as fresh - the response then carries `"rendered": false`.

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
  "stale_hours": 24,
  "stale_action": "dim",
  "stale_text": "STALE",
  "default_text": "waiting..."
}
```

## POST /api/value/config

Partial update: only the fields you send change.

| Field | Type | Meaning |
|---|---|---|
| `prefix` | string | Up to 16 characters, prepended to the value |
| `suffix` | string | Up to 16 characters, appended to the value |
| `decimals` | number | -1..9; -1 leaves the value exactly as sent, otherwise round half away from zero to that many decimals |
| `thousands` | string | Single separator character; `""` clears it. A space (`" "`) renders as a thin space, narrower than a typed space, so grouped values stay compact |
| `stale_hours` | number | 0..65535; 0 disables the staleness check |
| `stale_action` | string | `none`, `dim`, `marker` or `text` |
| `stale_text` | string | Up to 64 characters, shown by the `text` action |
| `default_text` | string | Up to 64 characters, shown verbatim in integration mode until the first value arrives; `""` keeps the panel blank |

```bash
curl -X POST http://192.168.1.50/api/value/config \
  -H "X-Api-Key: mysecret" \
  -d '{"prefix":"","suffix":" C","decimals":1,"stale_hours":6,"stale_action":"marker"}'
```

Response: `{"ok": true}`. Errors: 400 `"decimals must be -1..9"`, 400 `"stale_hours must be 0..65535"`, 400 `"stale_action must be none|dim|marker|text"`, 500 `"saving value config failed"`.

## Staleness

When no value arrives for `stale_hours`, the display flags it so a frozen number cannot masquerade as a live one:

| Action | Effect |
|---|---|
| `none` | No visual change (the webhook still fires) |
| `dim` | Halve the brightness until the next value arrives |
| `marker` | Blink a corner pixel |
| `text` | Replace the display with `stale_text` |

Crossing into staleness also fires the `value_stale` [webhook](../integrations/#outbound-webhook-payloads) with the age in seconds. The next `POST /api/value` clears the condition and restores brightness. `GET /api/status` reports `value_age_s`, seconds since the last value, -1 when none was ever received (see [system](../system/#get-apistatus)).

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
