---
title: Display control
description: Text, brightness, brightness cap, clear, test patterns, the legacy map shim and identify mode
sidebar:
  order: 2
---

Every POST on this page requires the `X-Api-Key` header when an API key is set (see [API conventions](../)); the one GET is open. Unless noted, errors are 400 `"bad json"` for an unparseable body and 401 for a missing or wrong key.

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/display/text` | Show text (partial update) |
| POST | `/api/display/brightness` | Set brightness, clamped to the cap |
| POST | `/api/display/brightness_cap` | Set the power-budget brightness cap |
| POST | `/api/display/power` | Configure the calculated current budget and its limiter |
| GET | `/api/display/light_sensor` | Ambient sensor config plus the live reading |
| POST | `/api/display/light_sensor` | Configure the ambient sensor (partial update) |
| POST | `/api/display/clear` | Blank the display and forget the stored text |
| POST | `/api/display/test` | Run or stop a calibration pattern |
| POST | `/api/display/map` | Legacy v1 map-mode shim |
| POST | `/api/display/identify` | Color every panel by chain index |
| POST | `/api/display/source` | Switch between static text and the integration value |

## POST /api/display/text

Shows text and persists it, so it survives a reboot. Every field is optional; omitted fields keep their current value, so a partial request from curl cannot silently reset the rest.

| Field | Type | Meaning |
|---|---|---|
| `text` | string | Up to 127 characters |
| `color` | object | `{"r":0..255,"g":0..255,"b":0..255}`; omitted channels keep their value |
| `scroll_mode` | string | `off`, `restart` (scrolls off, then re-enters) or `continuous` (marquee, the next copy follows two character cells behind the tail) |
| `scroll` | bool | Plain on/off switch, still accepted; `scroll_mode` supersedes it |
| `speed` | number | 0..255; scroll frame interval maps linearly from 200 ms (0) to 15 ms (255) |
| `font` | string | `5x7` (default), `3x5` (compact, uppercase letterforms) or `7x12` (tall, real lowercase) |
| `spacing` | string | `proportional` (default, blank glyph columns trimmed) or `monospace` (fixed cell); digits are fixed-width either way |
| `scale` | number | Integer font multiplier 1..8, clamped to what fits the display height; 0 or omitted keeps the current scale |

`scroll_mode` wins when both spellings are sent. On its own, `scroll` only switches scrolling off or on, so a client that always sends `scroll: true` cannot downgrade a configured continuous scroll to `restart`.

A request without a `text` field re-renders and persists everything else but leaves the stored text untouched. That matters in integration mode, where the on-screen text is the pushed value: an appearance-only POST must not overwrite the user's static text with it.

A font taller than the display falls back at render time to the tallest one that fits; the stored choice is kept and returns when a tall enough layout does.

```bash
curl -X POST http://192.168.1.50/api/display/text \
  -H "X-Api-Key: mysecret" -H "Content-Type: application/json" \
  -d '{"text":"12 DAYS LEFT","color":{"r":80,"g":32,"b":0},
       "scroll_mode":"continuous","font":"7x12","spacing":"proportional"}'
```

Response:

```json
{"ok": true}
```

Errors: 400 `"scroll_mode must be off|restart|continuous"`, 400 `"font must be 5x7|3x5|7x12"`, 400 `"spacing must be proportional|monospace"`.

## POST /api/display/brightness

Sets the global brightness. The value is clamped to 0..255 and then to the configured cap; a request above the cap is silently clamped rather than rejected, so clients written before a cap was configured keep working.

The value is always persisted; whether it reaches the panel right away is decided by the same arbiter the schedule and the light sensor feed into. Inside a scheduled-off window it takes effect at the next turn-on, and while the light sensor is driving the brightness it stays stored but unapplied. `GET /api/status` reports both: `display.brightness_stored` is this value, `display.brightness` is what the panel is running at.

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

## POST /api/display/power

Configures the [calculated power budget](../../architecture/power/): the constants that describe your panels, the current the LEDs may draw, and whether the driver enforces it. Every field is optional and omitted fields keep their stored value.

| Field | Type | Meaning |
|---|---|---|
| `limit_enabled` | bool | Enforce the budget. Off by default: with it off the figures are reported and nothing is changed |
| `budget_ma` | number | 0..25000; how much the LEDs alone may draw. 0 means no budget, and nothing is limited or reported. Default 2600 |
| `ua_per_channel` | number | 1..40000; microamps one colour channel draws at 255. Default 20000, the WS2812B datasheet nominal |
| `ua_quiescent` | number | 0..5000; microamps one LED draws with every channel off. Default 1000 |

```bash
curl -X POST http://192.168.1.50/api/display/power \
  -H "X-Api-Key: mysecret" -d '{"limit_enabled": true, "budget_ma": 2600}'
```

Errors: 400 when a field is out of range, and 400 `"budget_ma must be set before enabling the limiter"` for `limit_enabled` with a budget of 0, which would mean limiting the panel to nothing.

The live estimate is not here: it changes every frame, so it rides along in `GET /api/status` under `power`, where the web UI is already polling.

## GET /api/display/light_sensor

Open. Returns the [ambient light sensor](../../admin-guide/light-sensor/) configuration together with the live reading, which is what makes the two calibration points settable without a multimeter.

```bash
curl http://192.168.1.50/api/display/light_sensor
```

```json
{
  "enabled": true,
  "sda": 8,
  "scl": 9,
  "dark_raw": 200,
  "bright_raw": 2800,
  "min_brightness": 8,
  "max_brightness": 128,
  "gamma_x10": 10,
  "sampling": true,
  "raw": 1465,
  "level": 61,
  "raw_max": 4095,
  "valid_i2c_gpios": [1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
                      18, 19, 20, 21, 38, 39, 40, 41, 42, 47, 48]
}
```

| Field | Meaning |
|---|---|
| `enabled` | The sensor level replaces the stored brightness while the schedule allows the display on |
| `sda`, `scl` | The I2C pins. The v4 board's sensor connector reaches GPIO8 and GPIO9 |
| `dark_raw`, `bright_raw` | The two calibration readings in lux, 0..`raw_max`. Either may be the higher number, and the curve handles a reversed pair without a separate inversion setting |
| `min_brightness`, `max_brightness` | Brightness at `dark_raw` and at `bright_raw` |
| `gamma_x10` | Curve shape between the two ends, in tenths: 10 is a straight line, below 10 brightens early, above 10 stays dim longer |
| `sampling` | The sensor is stored **and** answering, even with `enabled` false, so calibration works before switching it on. A sensor that stops replying drops this to false while the config stays as it was |
| `raw` | Smoothed reading in lux, -1 until the first sample |
| `level` | Brightness the curve currently asks for, -1 when the sensor is not driving |
| `raw_max` | Top of the reading scale, 4095. Lux is clamped to it |
| `valid_i2c_gpios` | Pins `sda` and `scl` may use. Excludes the strapping pins 0, 3, 45 and 46, the GPIO22 to GPIO25 range the S3 does not bring out, and GPIO26 to GPIO37, which the module needs for its own flash and PSRAM |

## POST /api/display/light_sensor

Partial update: absent fields keep their stored value. Accepts `enabled`, `sda`, `scl`, `dark_raw`, `bright_raw`, `min_brightness` and `max_brightness`, plus `gamma_x10`, with the meanings above. Unknown fields are ignored. The config is persisted, the sensor is re-armed and the brightness is re-applied in one step.

Changing either pin throws the smoothed reading away, since the next sample comes off a bus that has just been rebuilt.

```bash
curl -X POST http://192.168.1.50/api/display/light_sensor \
  -H "X-Api-Key: mysecret" -H "Content-Type: application/json" \
  -d '{"sda":8,"scl":9,"dark_raw":5,"bright_raw":400,
       "min_brightness":8,"max_brightness":128,"enabled":true}'
```

Errors, all 400:

| Message | Cause |
|---|---|
| `sda/scl must be a free GPIO: not 0/3/45/46 (strapping), not 22-25 (absent), not 26-37 (flash and PSRAM)` | An I2C pin that cannot carry a bus on this module |
| `dark_raw/bright_raw must be 0-4095` | Reading out of range |
| `min_brightness/max_brightness must be 0-255` | Brightness out of range |
| `gamma_x10 must be 5-40 (0.5-4.0)` | Curve out of range |
| `dark_raw and bright_raw must differ, and SDA and SCL must be different pins` | A configuration that could not produce a brightness: a zero-width calibration window has no slope, and one pin cannot carry both bus lines |

Plus 500 `"saving light sensor config failed"` when the write to flash fails.

## POST /api/display/clear

Blanks the display and clears the persisted text. No request body is needed.

```bash
curl -X POST http://192.168.1.50/api/display/clear -H "X-Api-Key: mysecret"
```

Response: `{"ok": true}`

## POST /api/display/test

Runs a calibration or diagnostic pattern, or stops one. While a test is active the scroll task idles; leave test mode with `pattern: "off"`, by posting text, or by clearing.

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
| `off` | Ends whatever test or identify mode is running and restores the configured content (static text or integration value) |

```bash
curl -X POST http://192.168.1.50/api/display/test \
  -H "X-Api-Key: mysecret" -d '{"pattern": "border"}'
```

Response: `{"ok": true}`

## POST /api/display/map

Compatibility shim for the older map-mode API. Each mode name names a wiring preset, which is converted into the equivalent runtime layout. New clients should use [`POST /api/layout`](../layout/) instead.

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

## POST /api/display/source

Switches the [content source](../../user-guide/display/#content-source) and re-renders immediately: `static` shows the stored static text, `integration` shows the last pushed value, or the value config's `default_text` placeholder before the first push (blank when no placeholder is set). The choice is persisted; `GET /api/status` reports it as `display.source`.

| Field | Type | Meaning |
|---|---|---|
| `mode` | string | `static` or `integration`, required |

```bash
curl -X POST http://192.168.1.50/api/display/source \
  -H "X-Api-Key: mysecret" -d '{"mode": "integration"}'
```

Errors: 400 `"mode must be static|integration"`.
