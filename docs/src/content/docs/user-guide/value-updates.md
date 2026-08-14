---
title: Value updates
description: Push a number from any system and let the display format and show it
sidebar:
  order: 3
---

The value endpoint is how other systems drive the display. Anything that can send an HTTP POST with a JSON body can update it: an n8n or Node-RED flow, Home Assistant, a cron job with curl, a CI pipeline. The sender only delivers the raw value; the device owns the formatting (prefix, suffix, decimals, thousands separator), so every source stays a one-liner and the presentation is configured once, on the display.

The value appears in the current colour, size and [scroll mode](../display/#scrolling). Posting a value does not change any of those settings, so a display set to scroll keeps scrolling, which is what a long formatted value needs.

The value renders while the display's [content source](../display/#content-source) is Integration. In Static mode the POST is still accepted and remembered - switching to Integration shows the latest value - but the panel keeps the static text. The last value lives in RAM only: after a reboot the panel shows the configured placeholder (see [formatting](#formatting-configuration)) until the next POST arrives.

Values can also arrive over [MQTT](../../admin-guide/mqtt/) on `<base>/value/set`; they go through exactly the same formatting and freshness bookkeeping.

## Posting a value

POST a JSON body with a single `value` field, either a number or a string, to `/api/value`:

```sh
# a raw number
curl -X POST http://<device-ip>/api/value \
  -H 'X-Api-Key: YOUR_KEY' \
  -d '{"value": 12345678.9}'

# a string works the same
curl -X POST http://<device-ip>/api/value \
  -H 'X-Api-Key: YOUR_KEY' \
  -d '{"value": "12345678.9"}'
```

Drop the `X-Api-Key` header if no key is set (see [API key](#api-key)). The response echoes what ended up on the display:

```json
{"ok": true, "displayed": "CHF 12'345'678.9"}
```

Numbers and numeric strings behave the same. A string that does not parse as a number ("OPEN", "v2.0.0") is passed through untouched, with only the prefix and suffix added, so the endpoint also works for short status words.

Two details about numbers:

- JSON numbers are rendered with up to 10 significant digits and no trailing zeros, so `4.20` arrives as `4.2`. To keep trailing zeros, send the value as a string and leave decimals at -1, or set a fixed decimals count
- With a fixed decimals count, ties round away from zero: `2.5` at 0 decimals shows `3`

## Formatting configuration

The value config lives on the Integrations tab (advanced mode), or via `GET`/`POST /api/value/config`. Five fields shape the output:

| Field | Range | Effect |
|-------|-------|--------|
| Prefix | up to 16 characters | text before the value, e.g. `CHF ` |
| Suffix | up to 16 characters | text after the value, e.g. ` kg` |
| Decimals | -1 to 9 | fixed decimal places; -1 shows the value exactly as sent |
| Thousands separator | none, comma, dot, apostrophe, thin space | groups the integer digits in threes |
| Placeholder | up to 64 characters | shown verbatim in Integration mode until the first value arrives; empty keeps the panel blank |

The thin space renders narrower than a typed space - about the width of a dot - so a space-grouped value (`1 025 300`) takes barely more room than a dot-grouped one instead of overflowing the panel.

### Worked example

Config: prefix `CHF ` (note the trailing space), decimals 1, thousands separator apostrophe. Then:

```sh
curl -X POST http://<device-ip>/api/value -d '{"value": 12345678.9}'
```

1. Decimals 1 fixes the fraction: `12345678.9`
2. The separator groups the integer part: `12'345'678.9`
3. Prefix and suffix wrap it: `CHF 12'345'678.9`

The display shows `CHF 12'345'678.9`. The sender never dealt with any of it; if you later want two decimals or a different currency, you change the display config, not every flow that feeds it.

The Integrations tab has a test field that posts a value and shows the formatted result, which is the quickest way to check a config.

## The staleness guard

A display that silently shows an old number is worse than a blank one. If the flow that feeds an office display dies on a Friday, the Monday visitor reads Friday's figure as today's, and nobody notices because the display looks perfectly healthy. The staleness guard makes dead data visible.

Set "Stale after" to a number of hours (0 disables the guard, the UI accepts up to 720). When no value update has arrived for that long, the device applies the stale action:

| Action | What happens |
|--------|--------------|
| None | nothing visible; only the `value_stale` webhook fires |
| Dim | the brightness is halved, whatever set it: your slider, the [light sensor](../../admin-guide/light-sensor/), or the schedule's dim level. The halving comes after the brightness cap, so it stays visible on a capped display |
| Corner marker | a red pixel lights in the top-left corner |
| Show text | the display is replaced with the configured stale text |

Every action also fires the `value_stale` [webhook](#consuming-the-devices-webhooks) once, at the moment the value goes stale, with the age in seconds in the payload.

The next successful POST to `/api/value` clears everything: the brightness comes back, the marker and stale text disappear, and the timer restarts. The check runs every 30 seconds and counts device uptime, so it works without NTP; after a reboot the guard arms when the first update arrives.

Three situations pause the guard, because there is nothing useful to mark: a [schedule](../schedule/) off window that blanks the panel (a dimmed off window keeps the guard running), [setup-AP mode](../../getting-started/wifi-setup/), where the panel carries the setup SSID and address and must not be overwritten, and a [content source](../display/#content-source) set to Static - a stale value must not dim or overwrite text the user typed. All resume as soon as the integration value is back on the panel.

Pick the threshold from your update cadence with slack for normal gaps: a flow that posts every hour can go stale at 3 to 4 hours; one that posts once a day, at 26 to 30 hours.

## API key

When an API key is set on the Integrations tab, every POST (including `/api/value`) must carry it in the `X-Api-Key` header, or the device answers 401. Reads stay open. With no key set, all requests are allowed; that default is meant for a trusted home or office LAN, see the [admin guide](../../admin-guide/) for the security posture.

## Recipe: push a number with n8n

n8n is used here as the example, but nothing is n8n-specific: any tool that can make an HTTP request works the same way. The generic shape is always trigger, fetch, POST.

1. Trigger: a Schedule Trigger node (for example every 15 minutes). If your source can push instead, use a Webhook node and let the source call n8n
2. Fetch: a node that produces the number from your source, an HTTP Request node against an internal API, a Postgres/MySQL node, a spreadsheet node. Make sure the output item has a numeric field, say `number`
3. Send: an HTTP Request node
   - Method: POST
   - URL: `http://<device-ip>/api/value`
   - Body content type: JSON
   - Body: `{"value": {{ $json.number }}}`
   - Header: `X-Api-Key` with your key, if one is set
4. Activate the workflow

The response's `displayed` field tells you exactly what the display shows, which is handy in the execution log when a formatting config is off.

The same three steps in Node-RED are an inject node, a function or database node, and an http request node. In Home Assistant, a `rest_command` posting `{"value": "{{ states('sensor.your_number') }}"}` on an automation does it. Without any tooling, cron plus curl is enough:

```sh
# crontab: every 30 minutes
*/30 * * * * curl -s -X POST http://<device-ip>/api/value \
  -H 'X-Api-Key: YOUR_KEY' -d "{\"value\": $(your-command-that-prints-a-number)}"
```

## Consuming the device's webhooks

The device can also call you. On the Integrations tab, set an outbound webhook URL and tick the events you care about; the device POSTs a JSON envelope to that URL when they happen. Events: `boot`, `ip_change`, `ota_result`, `schedule_on`, `schedule_off`, `value_stale`.

The payload is always the same envelope, with an optional `data` object per event:

```json
{"event": "value_stale", "device": "led-display", "uptime_s": 86452,
 "data": {"age_s": 90000}}
```

`device` is the [device name](../../api-reference/system/#post-apisystemdevice_name) set on the System tab, so several displays can share one receiver. Delivery is fire and forget: 5-second timeout, one retry after 2 seconds, then the event is dropped, so treat webhooks as notifications, not as a reliable event log.

An n8n alerting flow for the two events worth waking up for:

1. Webhook node: method POST, copy its production URL into the device's webhook URL field
2. On the device, tick Boot and Value stale, save
3. In n8n, branch on the payload's `event` field with a Switch node
4. `value_stale` means the feeding flow is broken: send yourself a message with the `age_s` from `data`. `boot` after a power cut or crash is worth a note too

Any webhook receiver works in place of n8n: a Node-RED http in node, a Home Assistant webhook trigger, or a five-line HTTP handler.

## Related pages

- [Display](../display/): colour, brightness, and font size of the shown value
- [Schedule](../schedule/): off hours and how they pause the staleness guard
- [API reference](../../api-reference/): full request and response shapes
