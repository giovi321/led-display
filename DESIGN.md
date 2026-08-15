# Design: firmware v2

Target state for the second firmware generation: runtime-configurable panel
arrangement with a graphical editor, a restructured web UI with an advanced
mode, integrations (REST API with optional key, generic inbound value webhook,
outbound event webhooks), configuration backup/restore, OTA updates, and an
optional WireGuard client. This document is the reference for the
implementation; module names below are binding.

## 1. Pixel mapping engine

The compile-time `DISPLAY_WIDTH`/`DISPLAY_HEIGHT` macros and the
`led_map_mode_t` enum are replaced by a runtime model with two parts:

**Panel type** (one per display, all panels identical):

| Field | Values | Meaning |
|-------|--------|---------|
| `w`, `h` | 1..64 | pixels per panel, e.g. 32x8 |
| `origin` | `TL` `TR` `BL` `BR` | corner where the first LED sits |
| `axis` | `row` `col` | whether the strip walks rows or columns first |
| `serpentine` | bool | alternate direction every row/column |

This vocabulary covers every common WS2812B matrix sold (8x8, 16x16, 8x32
presets in the UI, plus custom w/h).

**Layout**: a grid of `rows x cols` cells (1..8 rows, 1..16 cols, max 32
panels, max 8192 total pixels). Each cell holds:

- `chain`: position of that panel in the physical daisy-chain (0..n-1),
  a permutation - every chain index appears exactly once
- `rot`: rotation of the physically mounted panel: 0 or 180 always;
  90/270 only when `w == h`

Logical display size is derived: `width = cols * w`, `height = rows * h`.

**LUT**: on every layout change the firmware rebuilds a
`uint16_t lut[width*height]` mapping logical `(x,y)` to strip index:
cell lookup, inverse rotation to panel-local coordinates, wiring function
(origin/axis/serpentine) to offset, `chain * w * h + offset`. Zero per-frame
cost. The LUT builder lives in a pure, allocation-free module
(`mapping.[hc]`) with host-side unit tests (plain gcc + assert, run by
`Firmware/test/host/run_tests.sh`), because mapping math is where this
project has historically burned time.

**Migration**: the legacy `map_mode` NVS key maps to equivalent layout
configs on first boot (e.g. `LED_MAP_GRID_2X2_COL_SERP` becomes the 2x2
layout with bottom row rot=180). `POST /api/display/map` stays as a
compatibility shim that installs the corresponding preset layout.

Frame buffer and scroll buffers move from static arrays to heap allocations
sized from the active layout. Refresh-rate note: WS2812B is ~30 us/LED, so
1024 px refreshes at ~33 Hz and the 8192 px cap at ~4 Hz; acceptable for a
static readout, documented in the UI when a large grid is configured.

## 2. Arrangement editor (web UI)

An SVG drawing in the Hardware section, not a form:

- grid rendered to scale; each cell shows its panel as a rounded rectangle
  with a chain-number badge and a wiring glyph (dot on the first-LED corner,
  arrows for direction)
- drag a panel onto another cell to swap them; tap/click a panel to rotate
  (0 -> 180 -> 0; square panels cycle 0 -> 90 -> 180 -> 270)
- a chain polyline snakes from panel 0 to n-1 across the drawing so the
  physical cable path is visible at a glance
- steppers for grid rows/cols; preset buttons 8x8 / 16x16 / 8x32 / custom
  w/h; panel wiring selector (origin, axis, serpentine) with live glyph
  preview
- Identify: `POST /api/display/identify` lights every physical panel in a
  distinct colour (chain 0 red, 1 green, 2 blue, 3 yellow, ...) and the
  editor paints the drawn panels the same way; the user drags the drawing
  until it matches the wall
- Apply: `POST /api/layout` -> firmware validates (permutation, bounds),
  rebuilds the LUT live, saves to NVS, and runs the border test pattern for
  visual confirmation

## 3. Web UI restructure and advanced mode

`web_ui.h` (C string literal) is replaced by real source files in
`Firmware/main/web/`: `index.html`, `style.css`, `app.js`. A CMake step
gzips them into the build dir and embeds via `EMBED_FILES`; the server sends
`Content-Encoding: gzip`. No frameworks, vanilla JS, one page.

- **Simple mode** (default): Display (text, colour picker + saved-colour
  chips, brightness, scroll, speed, scale), Schedule
- **Advanced mode** (header toggle, state in localStorage): adds Hardware
  (arrangement editor, test patterns, brightness cap), Network (WiFi,
  WireGuard), Integrations (API key, value endpoint config, outbound
  webhooks), System (OTA, backup/restore, boot screen, factory reset,
  reboot)

The HTTP layer splits into per-domain handler files: `api_display.c`,
`api_layout.c`, `api_system.c`, `api_wifi.c`, `api_integrations.c`,
`api_ota.c`, plus `web_server.c` reduced to server setup, static assets,
captive-portal probes, and a registration table each module contributes to.

## 4. Settings and persistence

NVS namespace `cfg` keeps existing scalar keys (text, colour, brightness,
scroll, speed, scale, boot screen). New scalar keys: `scm` (scroll mode,
section 11), `font`, `prop` (spacing, section 11), `dname` (device name;
absent means unset and every consumer falls back to its historical default)
and `dsrc` (display content source: 0 static text, 1 integration value;
absent means static). Structured state is stored as JSON blobs under their
own keys:

| Key | Content |
|-----|---------|
| `layout` | panel type + grid + cells (section 1) |
| `colors` | array of `{name, r, g, b}`, max 12 |
| `valuecfg` | prefix, suffix, decimals, thousands separator, stale_hours, stale_action, stale_text, default_text (integration-mode placeholder) |
| `integr` | api_key, webhook url, webhook event mask, the web UI login (`ui_auth`, `ui_user`, `ui_pass`), plus the legacy device-name copy `dname` superseded |
| `wg` | WireGuard enable, private key, peer public key, endpoint, port, allowed IPs, keepalive |
| `mqtt` | MQTT enable, broker URI, username, password, base topic, HA discovery flag + prefix |
| `lsen` | light sensor: enabled, gpio, dark_raw, bright_raw, min_bri, max_bri, gamma |

`settings.c` gains load/save helpers per blob; every blob has a version
field for forward migration.

Two namespaces outside `cfg`:

- `sched` holds the schedule scalars (`en`, `on_h`, `on_m`, `off_h`,
  `off_m`, `days`, `utc`) plus `oact` (off-window action), `obri` (the
  level the `dim` action holds) and `tz` (POSIX TZ override; empty falls
  back to the fixed `utc` offset); absent keys default to the blanking
  behaviour older builds had
- `wg` (the namespace, distinct from the `cfg`/`wg` blob) holds `crashes`,
  the consecutive-crash-reboot counter behind the WireGuard crash-loop
  guard (section 8)
- `wifi` holds five credential slots, `ssid0`..`ssid4` and
  `pass0`..`pass4`. Slot 0 is mirrored onto the single legacy `ssid`/`pass`
  pair so an OTA rollback still finds credentials, and a device that only
  has the legacy pair adopts it into slot 0 on the first boot

## 5. API surface (v2)

All existing endpoints remain. New/changed:

| Method | Path | Purpose |
|--------|------|---------|
| GET/POST | `/api/layout` | read/replace panel arrangement; POST rebuilds LUT live |
| POST | `/api/display/identify` | `{"on":true\|false}` identify mode |
| GET/POST | `/api/colors` | saved colour presets |
| POST | `/api/value` | `{"value":"4.2"}` or `{"value":4.2}`; device formats via valuecfg template and displays |
| GET/POST | `/api/value/config` | prefix/suffix/decimals/staleness settings |
| GET | `/api/config/export` | full config JSON (all blobs + scalars, WiFi credentials excluded; API key and WG private key included - endpoint requires the API key when one is set) |
| POST | `/api/config/import` | restore config, rebuild LUT, apply |
| POST | `/api/ota/upload` | raw `.bin` body -> inactive OTA slot |
| POST | `/api/ota/url` | `{"url":"https://..."}` pull update |
| GET | `/api/ota/status` | slot info, running version, last result |
| GET/POST | `/api/integrations` | API key, outbound webhook config |
| GET/POST | `/api/wireguard` | WireGuard config; status surfaced in `/api/status` |
| GET/POST | `/api/display/light_sensor` | ambient sensor config; the GET adds the live reading, the level and the pins that can be used |
| POST | `/api/system/device_name` | `{"name":".."}`; one name for hostname, setup-AP SSID and the webhook `device` field |
| GET | `/api/wifi/networks` | `{networks:[{ssid,connected}],count,max}`; passwords are never returned |
| POST | `/api/wifi/networks` | `{ssid,password}` adds or updates a slot; 409 when all five are in use |
| POST | `/api/wifi/networks/delete` | `{ssid}` forgets one network |
| POST | `/api/display/source` | `{"mode":"static"\|"integration"}` content source; re-renders immediately |
| GET/POST | `/api/mqtt` | MQTT client config (section 13); status surfaced in `/api/status` |

Changed shapes: `/api/display/text` accepts `scroll_mode`, `font` and
`spacing` (section 11), and a request without `text` re-renders but leaves
the stored text key alone (in integration mode the on-screen text is the
pushed value, which must not leak into the static text); `/api/schedule`
accepts `off_action` (`off`, `dim`), `off_brightness` (1..255) and `tz` (a
POSIX TZ override applied via `setenv`/`tzset`, DST included; empty keeps
the fixed `utc_offset`); `/api/display/test` accepts `pattern: "off"` to end
tests/identify and restore the configured content; `/api/value` renders only
in integration mode, remembers the formatted value in RAM either way, and
reports `rendered`; `/api/value/config` gains `default_text` (the
integration-mode placeholder before the first value); `/api/wifi/connect`
adds or updates a network and reboots instead of replacing the single stored
one; `/api/wifi/reset` forgets every network; `/api/wifi/scan` items gain
`saved`.

`/api/status` gains: firmware version + build date, layout summary, value
age (seconds since last `/api/value`), WireGuard state (`held` included),
MQTT state, OTA slot, `display.scroll_mode`/`font`/`spacing`,
`display.brightness_stored` next to the applied `display.brightness`,
`display.text_stored` next to the on-screen `display.text`,
`display.source`, `light_sensor` (`enabled`, `sampling`, `raw`, `level`),
`system.device_name` (resolved) and `system.device_name_stored` (empty when
unset), and `wifi.ap_ssid` in every mode.

**Auth**: if an API key is configured, every mutating endpoint (POST) checks
`X-Api-Key`; GETs stay open except `/api/config/export`. The WiFi endpoints
stay keyless in exactly one case: the setup AP of a device with no saved
network, which is genuine first-time provisioning where no key could have
been configured yet. A provisioned device that raised the setup AP after an
outage still requires the key, because that AP is open and the exception
would otherwise let a passer-by move the display onto their own network. The
consequence is deliberate: with a key set and then forgotten, WiFi cannot be
reconfigured from the fallback portal and recovery is a USB reflash. No key
configured = everything open (LAN-trust default), stated plainly in the docs.

**Cross-site hardening**: because the LAN-trust default leaves POSTs open,
`web_require_auth` also checks where the request claims to be going. A `Host`
header, and an `Origin` header when the browser sends one, must name the
device's WiFi IP, its WireGuard address, or its resolved device name
(optionally with one DNS suffix label such as `.local`, and an optional
`:port`). Anything else is 403 `cross-origin request rejected`, which closes
off DNS rebinding and cross-site POSTs from an unrelated page. Skipped in AP
mode, where the captive-portal sheet posts under the probe hostname it was
hijacked from.

**Value staleness**: `scheduler.c` grows a minute-tick check: if
`stale_hours > 0` and no value update for that long, apply `stale_action`:
`none`, `dim` (halve the level the schedule and the cap allow), `marker` (blinking
corner pixel), or `text` (show `stale_text`). Cleared by the next value post.
The guard only runs while the display source is `integration`: a stale value
must not dim, mark or overwrite the user's static text.

**Content source**: the `dsrc` scalar decides what the panel is wired to
show. Static: the stored text. Integration: the last value pushed via
`/api/value` or MQTT, held in RAM only (a reboot shows the `default_text`
placeholder until the next push - a stale-looking figure must never pose as
live data). Values pushed in static mode are still formatted, remembered and
counted fresh, so switching to integration shows the latest one. The switch,
the test-stop and the MQTT power-on all re-render through one helper
(`api_display_apply_source`).

**Brightness arbitration**: `scheduler.c` owns the one function that decides
what reaches the panel, in this order: the schedule level (inside the
on-window the stored user brightness, replaced by the ambient sensor's level
while the sensor is enabled and has read something; outside it 0 or
`off_brightness` per `off_action`), then the brightness cap, then the stale
halving. The cap is applied before the halving on purpose: clamping last
would swallow the staleness dim whenever the level is at least twice the cap,
because dimmed and undimmed would both land on the cap. Setup-AP mode is the
exception in all directions: the panel then carries the only copy of the AP
name and address, so the schedule does not blank it, the staleness guard
yields, and the sensor is bypassed in favour of the stored brightness.

`/api/status` reports both ends of this: `display.brightness` is what the
arbiter last applied (0 in a blanked off-window, halved when stale) and
`display.brightness_stored` is the user's level, so the UI slider can stay an
input without writing the applied value back.

## 6. Outbound webhooks

One configurable URL + per-event toggles, plain JSON POST
(`{"event":"...", "device":"...", "data":{...}}`), fire-and-forget with one
retry, 5 s timeout, from a low-priority task queue. Events: `boot`,
`ip_change`, `ota_result`, `schedule_on`, `schedule_off`, `value_stale`.
Consumable by n8n or anything else that accepts a webhook; nothing
n8n-specific in the firmware.

## 7. OTA updates

New partition table (16 MB flash): `nvs` unchanged at 0x9000 (existing
settings survive), `otadata`, `phy_init`, `ota_0` (6 MB), `ota_1` (6 MB),
`storage` NVS. The switch from the single `factory` app requires **one final
USB flash**; all subsequent updates go through the web UI or
`/api/ota/url`. Rollback: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`; the app
self-validates (`esp_ota_mark_app_valid_cancel_rollback`) only after the web
server is up, so a broken image reverts on next boot.

## 8. WireGuard client (optional, best effort)

Vendored `esp_wireguard` component (lwIP-based, upstream trombik/droscy)
under `Firmware/components/esp_wireguard/`, ported to ESP-IDF 6 (esp_netif
glue is the expected breakage). Kconfig option, compiled in by default but
runtime-disabled until configured. Config via Network section: private key,
peer public key, endpoint host:port, allowed IPs, persistent keepalive.
Status (handshake age, peer) in `/api/status`. If the IDF 6 port proves
unreasonable, the feature ships compile-time disabled and documented as
experimental; nothing else in this design depends on it.

Two hard lessons are baked in (2.3.0). First, every component call is
marshalled into the lwIP tcpip thread and the component's own rekey timer
runs there too, so `psa_crypto_init()` and the x25519/blake2s handshake
crypto all execute on the tcpip stack:
`CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=8192` is REQUIRED (the 3072 default
overflows into a stack-canary panic and a reboot loop on every connect).
Second, a tunnel bug must never brick the device out of its own web UI: the
`wg`-namespace `crashes` counter counts consecutive crash reboots
(panic/WDT reset reasons) with the tunnel enabled; at three the tunnel is
held down at boot (`held` in the status, banner in the UI) until the config
is re-saved. Any clean boot or completed handshake resets the counter.

## 9. Boot flow (v2)

settings -> layout/LUT -> display init (stored font + spacing) -> splash ->
WiFi -> integrations/webhooks -> boot screen -> SNTP/scheduler -> light
sensor -> WireGuard (if enabled) -> HTTP -> mark OTA image valid -> `boot`
webhook.

Ordering constraints inside that sequence:

- the hostname is set on both netifs before the radio starts, so the first
  DHCP request already carries it
- WiFi scans first, then walks the saved networks strongest-RSSI-first and
  finally the ones the scan missed (hidden SSIDs) in stored order, 10 s per
  attempt; with nothing reachable it raises the setup AP. Once boot ends in
  STA mode a reconnect task retries forever with a 2/5/15/30/60 s backoff and,
  after 30 min offline, raises the setup AP alongside the retries (AP+STA,
  every 5 min) until a reconnect tears it down again
- the boot screen is user-selectable except in AP mode, where the scrolling
  `Setup AP: <ssid> / <ip>` banner always wins - including when the AP is
  raised later, at runtime, by the reconnect task
- the light sensor starts after the scheduler: its first reading re-applies
  the brightness through the scheduler's arbiter (section 5), which needs its
  mutex to exist. Sampling runs whenever a valid pin is stored, even with the
  sensor disabled, so the UI can calibrate against a live reading

## 10. Security posture

Designed for a trusted LAN. Plain HTTP; the API key protects against casual
misuse, not a hostile network. Remote access is meant to go through
WireGuard rather than port-forwarding the device. Secrets (WiFi password,
API key, WireGuard private key) never appear in `/api/status` or logs; the
config export contains them and is key-protected when a key is set.

## 11. Text rendering (v2.2)

Three column-major pixel fonts in `fonts.[hc]` (pure C, host-tested):
`5x7` (default, the original set), `3x5` (compact, uppercase letterforms)
and `7x12` (tall, real lowercase with descenders). All cover ASCII 32..126;
integer scaling 1..8 clamped to the display height, and a font too tall for
the active layout falls back to the tallest one that fits (`font_fit`).

Spacing is proportional by default: blank edge columns are trimmed per
glyph, spaces advance `space_w`, and digits always advance `digit_w`
(tabular figures - a ticking value keeps a stable width). `monospace`
restores the fixed cell.

Scrolling has three modes (`display_scroll_t`): `off`, `restart` (text
exits fully before re-entering - the former scroll bool) and `continuous`
(marquee: the next repetition follows the tail after a two-character gap).

Wire/NVS vocabulary: `/api/display/text` accepts `scroll_mode`
(off|restart|continuous, wins over the legacy `scroll` bool), `font`
(5x7|3x5|7x12) and `spacing` (proportional|monospace); `/api/status` and
the config export report the same fields. NVS keys: `scm` (mode, `scrl`
kept in sync for rollback), `font`, `prop`.

One character outside the ASCII table: the thin space (0x1F,
`FONT_THIN_SPACE`), a blank advance one column narrower than the space in
both spacing modes. `valuefmt.c` emits it for the space thousands separator
so grouped values stay compact; typed text can never contain it, and the
renderer treats it as blank rather than the `?` fallback.

## 12. Out of scope (deferred)

Multiple parallel data pins, clock/countdown modes, HTTPS on-device,
per-panel type mixing (heterogeneous panels in one grid), an uploadable
image/logo as the integration placeholder (text only for now; a bitmap needs
its own storage and render path).

## 13. MQTT client (optional, v2.3)

`mqtt_link.c` on ESP-IDF's esp-mqtt (core component in IDF <6, the
`espressif/mqtt` managed component from 6.0). Kconfig `LEDDISP_MQTT`,
default y, no-op stubs when off - same contract as WireGuard. Config blob
in section 4; API `GET/POST /api/mqtt` with the WG secret rules (password
never echoed, empty keeps stored).

The device is an integration source and a controllable node under one base
topic (default: the device name): `text/set`, `value/set` (identical
semantics to the HTTP endpoints, same formatting and freshness bookkeeping),
`brightness/set`, `power/set` (blank/restore, RAM only), `source/set`,
`reboot/set`; retained `state` JSON every 30 s and after each command;
`availability` via LWT. Optional Home Assistant discovery publishes retained
configs (light with brightness, text, source select, reboot button, RSSI +
value-age diagnostics) under the discovery prefix, and withdraws them with
empty retained payloads when discovery is switched off. MQTT bypasses the
API key by design; the broker's auth is the boundary, stated in the docs.
