---
title: Value updates
description: Show a number from a URL, a POST or MQTT, with device-side formatting and a fallback for feeds that break
sidebar:
  order: 3
---

The display shows one value, and it can get that value three ways. The device can fetch it from a URL on an interval, any system that can send an HTTP POST can push it to `/api/value`, and a broker can deliver it over [MQTT](../../admin-guide/mqtt/) on `<base>/value/set`. All three go through the same formatting and the same [fallback](#fallback), so the presentation is configured once, on the display, and every source stays a one-liner.

The value appears in the current colour, size and [scroll mode](../display/#scrolling). A new value does not change any of those settings, so a display set to scroll keeps scrolling, which is what a long formatted value needs.

The value renders while the display's [content source](../display/#content-source) is Integration. In Static mode a value is still accepted and remembered (switching to Integration shows the latest one), but the panel keeps the static text. The last value lives in RAM only: after a reboot the panel shows the fallback text until the next value arrives.

On the Integrations tab (advanced mode) the cards follow the data flow: Value from URL, Value endpoint, MQTT, Formatting, Fallback, Status webhook and API key.

## Fetching the value from a URL

Use this when the source cannot push into your network, for example a cloud service. Enable "Value from URL" on the Integrations tab, enter the URL and save. The device then requests it on the interval you set and treats the result exactly like a pushed value: format, show, remember, count as fresh.

| Setting | Limits | Meaning |
|---------|--------|---------|
| Enabled | | polling runs only while this is on, a URL is set, and the device is on WiFi with an IP address |
| URL | `http://` or `https://`, up to 255 characters | HTTPS is verified against the built-in certificate bundle; redirects are followed |
| Interval | 10 to 86400 seconds, default 300 | time between polls |
| Value path | up to 64 characters | where the value sits in a JSON response; empty means the whole response body |
| Secret | up to 128 characters | sent as `Authorization: Bearer <secret>` |
| Custom header | name up to 48, value up to 128 characters | one extra request header |

Saving the card triggers one poll straight away, and the first poll after boot runs within a few seconds of the device getting an IP address. "Poll now" requests one more poll. The card shows the time of the last poll, the HTTP status, the value that was extracted or the error, and the number of failures in a row.

Polling runs on its own low-priority task. A slow or dead URL never blocks the web interface, the display or MQTT.

### Picking a value out of JSON

Set the value path to the field you want. A path is a list of object keys separated by dots, and `[n]` picks an array element, counting from 0. A path may start with an index.

For a response like this from `https://example.com/value.json`:

```json
{"data": {"price": 4210.5, "unit": "USD", "items": [{"value": 7}, {"value": 8}, {"value": 9}]}}
```

| Value path | Raw value |
|------------|-----------|
| `data.price` | `4210.5` |
| `data.unit` | `USD` |
| `data.items[2].value` | `9` |

For a response that is a bare array, `[0].price` takes the `price` of the first element. Keys that contain dots or brackets cannot be addressed. A string or number at the path is the raw value; numbers are taken exactly as written in the response. An object, array, true, false, null or a path that does not exist counts as a failure.

With the formatting set to prefix `USD `, 1 decimal and a comma as thousands separator, `data.price` shows `USD 4,210.5`.

### Showing a plain-text response

Leave the value path empty and the whole response body, with surrounding whitespace trimmed, is the raw value. A URL that answers with just `21.4` gives the raw value `21.4`. When the body is not a number, it is shown exactly as received, with only the prefix and suffix added, so a URL that answers `OPEN` puts `OPEN` on the panel. The body is capped at 4 KB; anything larger is a failure.

### Secret or custom header

Both are optional and can be set at the same time.

- Secret: for services that expect a bearer token. The device sends `Authorization: Bearer <secret>`
- Custom header: for services that expect a key in a header of their own, such as `X-Token`. Give the name and the value; the device sends `<name>: <value>`. The name must be a valid HTTP header name (letters, digits and the characters `!#$%&'*+-.^_`|~`) and the value must be a single line

Neither is shown again after saving. The interface and the API report only that a secret or header value is set. Saving the card with those fields left blank keeps the stored values, and the Clear buttons remove them (Clear on the custom header removes the name and the value). Prefer a secret or header over credentials inside the URL: the URL is shown in the card and travels in every backup in cleartext, as the secrets do, so store backups like secrets either way.

### What counts as a failed poll

A poll fails when any of these happens:

- the connection cannot be made, TLS verification fails, or the server gives no answer within 10 seconds
- the HTTP status is not 2xx after redirects
- the response is empty, or larger than 4 KB
- the value path is set and the response is not JSON, the path does not exist, or the target is not a string or number

The error text shows on the card. A failed poll changes nothing on the panel by itself: the last value stays. Only a run of failures engages the [fallback](#fallback), and the default is three in a row. A successful poll resets the count.

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

- JSON numbers are rendered with up to 15 significant digits and no trailing zeros, so `4.20` arrives as `4.2`. To keep trailing zeros, send the value as a string and leave decimals at -1, or set a fixed decimals count
- With a fixed decimals count, ties round away from zero: `2.5` at 0 decimals shows `3`

## Formatting

The Formatting card applies to every source: URL, value endpoint and MQTT. The same settings are available through `GET`/`POST /api/value/config`.

A numeric value passes through these steps in this order:

1. Parse. The whole string must be a number. Anything else is not formatted and shows as received, with only the prefix and suffix
2. Multiply by the multiplier (default 1), for unit conversion or 100 for a percentage
3. Scale: none, auto, k, M or B. The number is divided by 1000, 1000000 or 1000000000 and a letter is appended. Auto picks the largest of k, M, B and T that fits (a value below 1000 gets no letter)
4. Round to the number of decimals, half away from zero. -1 keeps the number as sent when there is no multiplier or scale
5. Thousands separator and decimal separator
6. Plus sign on positive numbers, if enabled
7. Prefix and suffix

| Setting | Range | Effect |
|---------|-------|--------|
| Prefix | up to 16 characters | text before the value, e.g. `CHF ` |
| Suffix | up to 16 characters | text after the value, e.g. ` kg` |
| Multiplier | any non-zero number | applied before everything else |
| Scale | none, auto, k, M, B | divides by 1000 per step and appends the letter |
| Scale letters | up to 23 characters | four space-separated tokens for k, M, B and T, default `k M B T`. A missing token falls back to its default letter |
| Decimals | -1 to 6 | fixed decimal places; -1 shows the number as sent |
| Decimal separator | dot or comma | the character between the integer and the fraction |
| Thousands separator | none, comma, dot, apostrophe, thin space | groups the integer digits in threes. It cannot be the same character as the decimal separator |
| Plus sign | on or off | puts `+` in front of positive numbers |

The thin space renders narrower than a typed space, about the width of a dot, so a space-grouped value (`1 025 300`) takes barely more room than a dot-grouped one instead of overflowing the panel.

When rounding carries a value into the next unit, auto scale moves up with it: `999.96k` at 1 decimal shows `1.0M`, never `1000.0k`.

### Worked examples

Config: prefix `USD ` (note the trailing space), scale M, decimals 1, thousands separator comma, decimal separator dot. A source sends the raw value `1234567.891`:

1. Multiplier 1 changes nothing
2. Scale M divides by one million: `1.234567891`
3. Decimals 1 rounds it: `1.2`
4. The thousands separator groups the integer part: `1.2`
5. The scale letter follows the number and the prefix goes in front: `USD 1.2M`

More configurations, each with its own raw value:

| Raw | Config | Shown |
|-----|--------|-------|
| `1234.5` | decimals 2, thousands dot, decimal separator comma | `1.234,50` |
| `0.0734` | multiplier 100, decimals 1, suffix `%` | `7.3%` |
| `2500000` | scale auto, decimals 1 | `2.5M` |
| `12345678.9` | prefix `CHF `, decimals 1, thousands apostrophe | `CHF 12'345'678.9` |

The sender never deals with any of it. If you later want two decimals or a different currency, you change the display config, not every flow that feeds it.

The Formatting card has a preview field. Type a raw value and the card shows what the current, even unsaved, settings would display. The preview asks the device to format it, and nothing is shown on the panel or stored. The same dry run is available as [`POST /api/value/preview`](../../api-reference/value-and-colors/#post-apivaluepreview). The Value endpoint card has a test field that posts a real value, which is the quickest way to check the whole path.

## Fallback

A display that silently shows an old number is worse than a blank one. If the flow that feeds an office display dies on a Friday, the Monday visitor reads Friday's figure as today's, and nobody notices because the display looks perfectly healthy. The fallback makes a broken feed visible and, if you want, tells you about it. This section is the one place that describes it; other pages link here.

### What the panel does

The fallback has a text of up to 64 characters, shown verbatim without formatting, and a mode:

| Mode | What happens while the fallback is engaged |
|------|--------------------------------------------|
| Show text | the panel shows the fallback text |
| Corner marker | the last value stays and a red pixel lights in the top-left corner |
| Dim | the last value stays and the brightness is halved, whatever set it: your slider, the [light sensor](../../admin-guide/light-sensor/) or the schedule's dim level. The halving comes after the brightness cap, so it stays visible on a capped display |

Before the first value since boot there is no value to mark or dim, so the panel shows the fallback text whatever the mode. With an empty fallback text the panel is blank. This is what you see for the first seconds after a reboot while the value is still on its way.

### What engages it

Any one of these engages the fallback:

| Trigger | Setting | Reported reason |
|---------|---------|-----------------|
| No value for a number of minutes | "After minutes without a value", 1 to 43200, 0 turns it off | `timeout` |
| A number of consecutive failed polls | "Poll failures", 0 turns it off, default 3. Counts only while Value from URL is enabled | `poll_failures` |
| The MQTT broker is lost for more than 60 seconds | "Engage when the MQTT broker is lost", off by default. Counts only while MQTT is enabled | `mqtt_lost` |

The timeout counts from boot while no value has arrived, so a device whose feed never starts also reaches it. All timing uses device uptime, so it works without NTP.

The next successful value from any source clears the fallback: a poll, a POST or an MQTT message. Triggers are events, not conditions that are checked again every 30 seconds:

- A value from any source clears the fallback, resets the failure count to 0 and re-arms every trigger
- A trigger can fire again only on a new event: the failure count reaching the threshold again from 0, or a fresh MQTT disconnect that lasts 60 seconds. A poll that keeps failing while MQTT delivers values therefore does not switch the fallback back on at every tick
- One slow or failed poll below the threshold changes nothing, so the fallback does not flap
- No value since boot changes the panel only. It sends no notification and the MQTT `fallback` flag stays false. Notification starts when one of the triggers above fires

The panel changes (text, marker, dim) only while the [content source](../display/#content-source) is Integration, and switching from Static to Integration shows the current fallback state straight away. Everything else runs whatever the source: the triggers, the webhooks, the MQTT flag and the heartbeat field, because they tell a remote watcher whether the feed is healthy, which is true regardless of what the panel shows.

Two situations differ. In [setup-AP mode](../../getting-started/wifi-setup/) the periodic checks and the dim stand down, because the panel carries the setup SSID and address and must not be overwritten. A [schedule](../schedule/) off window that blanks the panel keeps the display dark whatever the fallback says; the triggers and notifications carry on.

### How it is reported

- Webhook: `value_stale` fires once when the fallback engages, with the reason and `age_s`, the seconds since the last value (-1 when none has arrived since boot). `value_recovered` fires when it clears, with the reason that was active. Both are selectable like the other events. See the [payloads](../../api-reference/integrations/#outbound-webhook-payloads)
- MQTT: the state JSON has `fallback` and `fallback_reason`, and Home Assistant gets a "Value fallback" binary sensor. See the [MQTT guide](../../admin-guide/mqtt/)
- Heartbeat: the `status` event carries `fallback` and `fallback_reason` too
- `GET /api/status` reports `value_age_s`, seconds since the last value from any source, -1 when none was ever received (see [system](../../api-reference/system/#get-apistatus))

### Choosing the settings

Pick the timeout from your update cadence with slack for normal gaps: a source that updates every hour can go quiet at 3 to 4 hours before it is a problem; one that updates once a day, at 26 to 30 hours. With polling on, the failure count reacts much sooner than the timeout: three failures at a 5 minute interval is about 15 minutes. Use the timeout as the safety net for sources that push, and the failure count for a URL.

The settings are on the Fallback card, or the `fb_*` fields of [`/api/value/config`](../../api-reference/value-and-colors/#post-apivalueconfig).

## API key

When an API key is set on the Integrations tab, every POST must carry it in the `X-Api-Key` header, or the device answers 401. That includes `/api/value`, `/api/value/preview` and the poll endpoints. Reads stay open. With no key set, all requests are allowed; that default is meant for a trusted home or office LAN, see the [admin guide](../../admin-guide/) for the security posture.

## Recipe: push a number with n8n

n8n is used here as the example, but nothing is n8n-specific: any tool that can make an HTTP request works the same way. The generic shape is always trigger, fetch, POST. If the source can be reached from the device, [fetching the value from a URL](#fetching-the-value-from-a-url) needs no flow at all.

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

The device can also call you. On the Integrations tab, set the Status webhook URL and tick the events you care about; the device POSTs a JSON envelope to that URL when they happen. Events: `boot`, `ip_change`, `ota_result`, `schedule_on`, `schedule_off`, `value_stale` and `value_recovered`. With a heartbeat interval set, the device also posts a `status` event on that interval, whichever events are ticked.

The payload is always the same envelope, with an optional `data` object per event:

```json
{"event": "value_stale", "device": "led-display", "uptime_s": 86452,
 "data": {"reason": "timeout", "age_s": 90000}}
```

`device` is the [device name](../../api-reference/system/#post-apisystemdevice_name) set on the System tab, so several displays can share one receiver. The card can also send a secret as `Authorization: Bearer` and one custom header with every webhook, under the same rules as the [URL poll](#secret-or-custom-header). Delivery is fire and forget: 5-second timeout, one retry after 2 seconds, then the event is dropped, so treat webhooks as notifications, not as a reliable event log. The full payload of each event is in the [API reference](../../api-reference/integrations/#outbound-webhook-payloads).

An n8n alerting flow for the events worth waking up for:

1. Webhook node: method POST, copy its production URL into the device's webhook URL field
2. On the device, tick Boot, Value stale and Value recovered, save
3. In n8n, branch on the payload's `event` field with a Switch node
4. `value_stale` means the feed is broken: send yourself a message with the `reason` and `age_s` from `data`. `value_recovered` closes it. `boot` after a power cut or crash is worth a note too

For a dead-man's switch, set a heartbeat of, say, 15 minutes and alert in the receiver when no `status` event has arrived for twice that. A device that lost power or its network cannot report its own absence, so only the silence tells you.

Any webhook receiver works in place of n8n: a Node-RED http in node, a Home Assistant webhook trigger, or a five-line HTTP handler.

## Upgrading from 2.11.0

Version 2.12.0 replaces the stale settings and the placeholder text with the [fallback](#fallback). Nothing needs doing on upgrade. On the first boot the device converts the old settings, and a 2.11.0 backup imports through the same conversion.

| 2.11.0 setting | Becomes |
|----------------|---------|
| Placeholder (`default_text`), if set | the fallback text |
| Stale text (`stale_text`), when there was no placeholder | the fallback text |
| Stale after (`stale_hours`) | "After minutes without a value", hours times 60 (at most 43200) |
| Stale action text, marker or dim | the fallback mode, unchanged |
| Stale action none | "After minutes" 0 and mode text |

Three differences you can notice:

- The fallback text is one text for both jobs. If you had a placeholder and a different stale text, the placeholder wins
- The timeout now counts from boot while no value has arrived, where the old guard armed only after the first value
- A configuration that used a dot as thousands separator now gets a comma as decimal separator, because the two can no longer be the same character. Check the preview if you relied on a dot for both

The decimals range is now -1 to 6 instead of -1 to 9; a stored larger number is clamped to 6. Scripts keep working: `GET /api/value/config` still returns `stale_hours`, `stale_action`, `stale_text` and `default_text` for one release, derived from the new settings, and `POST /api/value/config` still accepts them. Use the `fb_*` fields in new scripts.

## Related pages

- [Display](../display/): colour, brightness, and font size of the shown value
- [Schedule](../schedule/): off hours and how they interact with the fallback
- [MQTT](../../admin-guide/mqtt/): the broker connection and Home Assistant entities
- [API reference](../../api-reference/): full request and response shapes
