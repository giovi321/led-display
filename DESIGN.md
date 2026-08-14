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
scroll, speed, scale, boot screen, schedule). New structured state is stored
as JSON blobs under new keys:

| Key | Content |
|-----|---------|
| `layout` | panel type + grid + cells (section 1) |
| `colors` | array of `{name, r, g, b}`, max 12 |
| `valuecfg` | prefix, suffix, decimals, thousands separator, stale_hours, stale_action, stale_text |
| `integr` | api_key, webhook url, webhook event mask |
| `wg` | WireGuard enable, private key, peer public key, endpoint, port, allowed IPs, keepalive |

`settings.c` gains load/save helpers per blob; every blob has a version
field for forward migration.

## 5. API surface (v2)

All existing endpoints remain. New/changed:

| Method | Path | Purpose |
|--------|------|---------|
| GET/POST | `/api/layout` | read/replace panel arrangement; POST rebuilds LUT live |
| POST | `/api/display/identify` | `{"on":true\|false}` identify mode |
| GET/POST | `/api/colors` | saved colour presets |
| POST | `/api/value` | `{"value":"4.2"}` or `{"value":4.2}`; device formats via valuecfg template and displays |
| GET/POST | `/api/value/config` | prefix/suffix/decimals/staleness settings |
| GET | `/api/config/export` | full config JSON (all blobs + scalars + wifi ssid; secrets included - endpoint requires API key when one is set) |
| POST | `/api/config/import` | restore config, rebuild LUT, apply |
| POST | `/api/ota/upload` | raw `.bin` body -> inactive OTA slot |
| POST | `/api/ota/url` | `{"url":"https://..."}` pull update |
| GET | `/api/ota/status` | slot info, running version, last result |
| GET/POST | `/api/integrations` | API key, outbound webhook config |
| GET/POST | `/api/wireguard` | WireGuard config; status surfaced in `/api/status` |

`/api/status` gains: firmware version + build date, layout summary, value
age (seconds since last `/api/value`), WireGuard state, OTA slot.

**Auth**: if an API key is configured, every mutating endpoint (POST) checks
`X-Api-Key`; GETs stay open except `/api/config/export`. In AP/captive-portal
setup mode the WiFi endpoints stay keyless. No key configured = everything
open (LAN-trust default), stated plainly in the docs.

**Value staleness**: `scheduler.c` grows a minute-tick check: if
`stale_hours > 0` and no value update for that long, apply `stale_action`:
`none`, `dim` (halve brightness), `marker` (blinking corner pixel), or
`text` (show `stale_text`). Cleared by the next value post.

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

## 9. Boot flow (v2)

settings -> layout/LUT -> display init -> splash -> WiFi -> SNTP/scheduler
-> WireGuard (if enabled) -> HTTP -> mark OTA image valid -> `boot` webhook.

## 10. Security posture

Designed for a trusted LAN. Plain HTTP; the API key protects against casual
misuse, not a hostile network. Remote access is meant to go through
WireGuard rather than port-forwarding the device. Secrets (WiFi password,
API key, WireGuard private key) never appear in `/api/status` or logs; the
config export contains them and is key-protected when a key is set.

## 11. Out of scope (deferred)

Multiple parallel data pins, MQTT, additional fonts, clock/countdown modes,
HTTPS on-device, per-panel type mixing (heterogeneous panels in one grid).
