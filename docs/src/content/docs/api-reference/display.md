---
title: Display control
description: Text, brightness, brightness cap, clear, test patterns, the legacy map shim and identify mode
sidebar:
  order: 2
---

All endpoints on this page are POST and require the `X-Api-Key` header when an API key is set (see [API conventions](../)). Unless noted, errors are 400 `"bad json"` for an unparseable body and 401 for a missing or wrong key.

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/display/text` | Show text (partial update) |
| POST | `/api/display/brightness` | Set brightness, clamped to the cap |
| POST | `/api/display/brightness_cap` | Set the power-budget brightness cap |
| POST | `/api/display/clear` | Blank the display and forget the stored text |
| POST | `/api/display/test` | Run a calibration pattern |
| POST | `/api/display/map` | Legacy v1 map-mode shim |
| POST | `/api/display/identify` | Color every panel by chain index |

## POST /api/display/text

Shows text and persists it, so it survives a reboot. Every field is optional; omitted fields keep their current value, so a partial request from curl cannot silently reset the rest.

| Field | Type | Meaning |
|---|---|---|
| `text` | string | Up to 127 characters |
| `color` | object | `{"r":0..255,"g":0..255,"b":0..255}`; omitted channels keep their value |
| `scroll` | bool | Scroll horizontally instead of static display |
| `speed` | number | 0..255; scroll frame interval maps linearly from 200 ms (0) to 15 ms (255) |
| `scale` | number | Integer font multiplier 1..8, clamped to what fits the display height; 0 or omitted keeps the current scale |

```bash
curl -X POST http://192.168.1.50/api/display/text \
  -H "X-Api-Key: mysecret" -H "Content-Type: application/json" \
  -d '{"text":"12 DAYS LEFT","color":{"r":80,"g":32,"b":0},"scroll":false}'
```

Response:

```json
{"ok": true}
```

## POST /api/display/brightness

Sets the global brightness. The value is clamped to 0..255 and then to the configured cap; a request above the cap is silently clamped rather than rejected, so clients written before a cap was configured keep working. The result is persisted and the frame is refreshed immediately.

```bash
curl -X POST http://192.168.1.50/api/display/brightness \
  -H "X-Api-Key: mysecret" -d '{"brightness": 60}'
```

Response: `{"ok": true}`. A body without a numeric `brightness` field changes nothing but still returns `{"ok": true}`.

## POST /api/display/brightness_cap

Sets the power-budget guard. Brightness requests above the cap are clamped from then on, and if the cap lands below the current brightness the display dims immediately.

| Field | Type | Meaning |
|---|---|---|
| `cap` | number | 0..255, required |

```bash
curl -X POST http://192.168.1.50/api/display/brightness_cap \
  -H "X-Api-Key: mysecret" -d '{"cap": 128}'
```

Errors: 400 `"cap required"` when the field is missing or not a number.

## POST /api/display/clear

Blanks the display and clears the persisted text. No request body is needed.

```bash
curl -X POST http://192.168.1.50/api/display/clear -H "X-Api-Key: mysecret"
```

Response: `{"ok": true}`

## POST /api/display/test

Runs a calibration or diagnostic pattern. While a test is active the scroll task idles; leave test mode by posting text or clearing.

| Field | Type | Meaning |
|---|---|---|
| `pattern` | string | One of the names below; missing or unknown names fall back to `step_raw` |

| Pattern | What it shows |
|---|---|
| `step_raw` | Walks strip indices 0..255 one LED at a time, ignoring the mapping (find your wiring order) |
| `step_xy` | Walks logical coordinates using the mapping (verify the layout) |
| `corners` | Lights the four logical corners in distinct colors |
| `border` | Outline of the whole display using the mapping |
| `fill` | Every LED on in the default amber |

```bash
curl -X POST http://192.168.1.50/api/display/test \
  -H "X-Api-Key: mysecret" -d '{"pattern": "border"}'
```

Response: `{"ok": true}`

## POST /api/display/map

Compatibility shim for the v1 map-mode API. The mode names map onto the legacy wiring presets, which are converted into equivalent runtime layouts. New clients should use [`POST /api/layout`](../layout/) instead.

| Field | Type | Meaning |
|---|---|---|
| `mode` | string | `row`, `serp`, `row_bl`, `serp_bl`, `col`, `col_serp` (single-panel presets) or `grid_2x2_col_serp` (the 2x2 grid) |

The new layout is applied, persisted, and the current text is re-rendered under it.

```bash
curl -X POST http://192.168.1.50/api/display/map \
  -H "X-Api-Key: mysecret" -d '{"mode": "grid_2x2_col_serp"}'
```

Errors: 400 `"unknown mode"`, 500 `"reconfigure failed"`.

## POST /api/display/identify

Identify mode for the arrangement editor: every panel lights up in a distinct color keyed by its chain index (0 red, 1 green, 2 blue, 3 yellow, 4 magenta, 5 cyan, 6 orange, 7 white, then the palette repeats). The editor paints its drawn panels with the same palette, so you can match physical panels to grid cells by eye.

| Field | Type | Meaning |
|---|---|---|
| `on` | bool | Required; `false` restores the stored text |

```bash
curl -X POST http://192.168.1.50/api/display/identify \
  -H "X-Api-Key: mysecret" -d '{"on": true}'
```

Errors: 400 `"on required"` when the field is missing or not a boolean.
