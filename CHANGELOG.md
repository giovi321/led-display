# Changelog

All notable changes to the firmware are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.2.0] - 2026-08-14

### Added

- Three text fonts: the original `5x7`, a compact `3x5` with uppercase letterforms, and a tall `7x12` with real lowercase; `POST /api/display/text` takes `font`, and the Display tab offers only the pixel sizes that fit the display height
- Proportional spacing, now the default: the blank columns around narrow letters are trimmed, while digits keep a fixed width so a ticking value never wobbles; `spacing: "monospace"` restores the fixed cell
- Continuous scrolling: the text repeats after a gap of two character cells instead of clearing the display first, selected with `scroll_mode` (`off`, `restart`, `continuous`)
- Schedule off-window action: `off` blanks the panel as before, `dim` holds it at a configurable `off_brightness` so the value stays readable outside working hours
- Up to 5 saved WiFi networks: `GET/POST /api/wifi/networks` and `POST /api/wifi/networks/delete` manage the list, `/api/wifi/scan` marks the ones already saved, and the Network tab can add a network without dropping the current connection. At boot the device scans and joins the strongest saved network in range, then tries the rest, hidden ones included
- Runtime reconnect: the saved networks are retried forever with a growing backoff (2 s to 60 s), in every situation including a boot where nothing was reachable. Such a boot raises the setup AP immediately and keeps retrying underneath it, so a router that boots slower than the display recovers on its own, and the portal's Scan button now works there. Retries slow to one every 5 minutes only while somebody is connected to the portal
- Optional ambient-light sensor: an LDR voltage divider or analog lux module on an ADC1 pin sets the brightness from the room, calibrated against the live reading in the Hardware tab, `GET/POST /api/display/light_sensor`. A divider that reads high in the dark just yields a reversed calibration pair, which the curve handles, so there is no inversion setting
- Device name, `POST /api/system/device_name`: one name drives the network hostname, the setup-AP SSID and the `device` field of outbound webhooks, and the web UI header and browser tab show it
- Documentation site: an assembly guide covering the build of the reference display from the cut and printed parts in `3D/`, a light sensor guide, and a releasing guide for maintainers

### Changed

- `GET /api/status` reports `display.scroll_mode`, `display.font`, `display.spacing`, `display.brightness_stored`, `light_sensor`, `system.device_name`, `system.device_name_stored` and `wifi.ap_ssid`; the config export and import carry the new display, schedule and light-sensor fields, and backups written by an older build still import
- The legacy `scroll` bool on `POST /api/display/text` keeps working but only switches scrolling on or off, so a client written before 2.2.0 cannot downgrade a continuous scroll
- `POST /api/wifi/connect` adds or updates a network instead of replacing the stored one, and `POST /api/wifi/reset` forgets all of them; a single network stored by an earlier build moves into the first slot automatically
- In setup-AP mode the panels always scroll `Setup AP: <ssid> / <ip>`, whatever the boot screen is set to, and neither the schedule nor the staleness guard may blank or dim it away
- Once the light sensor has produced a level, the Display slider reads it out instead of setting it; it stays an input before the first reading and in setup-AP mode, where the sensor is bypassed. The stored level is untouched and comes back when the sensor is switched off, and `/api/status` reports it as `display.brightness_stored` next to the applied `display.brightness`
- The device name moved from the Integrations tab to System; `POST /api/integrations` still accepts `device_name` and writes the same value, and a name set by an older build is adopted once unless it cleans to the default. Browsing to `http://<device-name>/` works for writes as well as reads
- Brightness order: the schedule level is clamped by the brightness cap and only then halved by the staleness guard, so a stale value stays visibly dimmer on a capped display
- `POST /api/value` shows the value in the current scroll mode and speed instead of forcing static text, and no longer overwrites the stored scroll mode
- The WiFi endpoints are keyless in one case only: the setup AP of a device with no saved network. A provisioned device on its fallback AP requires the API key like everywhere else, so a set-and-forgotten key means WiFi can no longer be reconfigured from the portal and recovery is a USB reflash
- Requests that change something must address the device by its own IP, WireGuard address or device name (one DNS suffix label and a port are allowed); anything else gets 403 `cross-origin request rejected`. That closes off DNS rebinding and cross-site POSTs in the default configuration where no API key is set
- `POST /api/schedule` answers 500 `schedule applied but not saved` when the new schedule is live but the write to flash failed, and `POST /api/system/boot_screen` answers 500 `saving boot screen failed`, instead of both reporting success
- The Text size list keeps a stored size that no longer fits the display, labelled as taller than the display, so Apply still round-trips after the layout shrinks
- A light-sensor section naming a pin outside GPIO1 to GPIO10 is reported as skipped on config import rather than clamped onto another pin
- Arrangement editor: every panel carries the first-LED corner dot and a rotation chip, the chain line lost its arrowheads, and the wiring controls are labelled in plain language (First LED, Strip runs, Zigzag) with an explanation behind each `?`
- Header redesign: connection dot, network name above the IP address, firmware version, and the device name
- Release workflow: the version check survives a CRLF checkout and says what to fix, a missing `led_display.bin` fails the run instead of publishing a release devices cannot use, a tag with a suffix (`v2.2.0-rc1`) publishes as a prerelease, and the workflow can be run manually for a tag that was already pushed
- The documentation site builds on Node 22, which Astro requires
- `3D/` reorganised around panel size: the frame parts of the reference build move under `3D/8x32/`, the controller enclosure becomes `Box for PCB_v2.stl`, and the SketchUp sources are no longer tracked, only the DXF and STL exports
- `docs.md` removed: it described the v1 firmware, and the documentation site covers the same ground for v2

## [2.1.0] - 2026-08-14

### Added

- Update from GitHub releases: `GET /api/ota/check` queries the latest release of the configured repository (`CONFIG_LEDDISP_GITHUB_REPO`), compares it with the running version and returns the `led_display.bin` asset URL; the System tab gets a "Check GitHub for updates" button with one-click install through the existing URL updater
- Release workflow: pushing a tag `vX.Y.Z` builds the firmware in CI (tag must match `FW_VERSION`) and attaches `led_display.bin` to the GitHub release

## [2.0.0] - 2026-08-14

### Added

- Runtime panel arrangement: one panel description (size, first-LED corner, row or column walk, serpentine) plus a grid of cells with chain index and rotation, up to 8x16 cells, 32 panels, 8192 pixels; the lookup table rebuilds live on every layout change
- Graphical arrangement editor in the web UI: drag panels to swap, click to rotate, chain polyline showing the cable path, presets for common panel sizes, identify mode that lights each physical panel in a distinct colour
- Advanced mode in the web UI: header toggle that unlocks the Hardware, Network, Integrations and System tabs on top of the simple Display and Schedule view
- Value endpoint `POST /api/value`: accepts a number or string, formats it on the device (prefix, suffix, decimals, thousands separator) and displays it; staleness guard with configurable timeout and action (none, dim, corner marker, fallback text)
- Saved colour presets, up to 12 named entries, `GET/POST /api/colors`
- Optional API key: when set, every POST requires the `X-Api-Key` header; WiFi endpoints stay keyless in captive-portal setup mode
- Outbound webhooks: one URL with per-event toggles for boot, ip_change, ota_result, schedule_on, schedule_off and value_stale; plain JSON POST, fire-and-forget with one retry
- OTA updates: raw `.bin` upload via `POST /api/ota/upload`, pull from URL via `POST /api/ota/url`, progress and result in `GET /api/ota/status`, automatic rollback if the new image fails to self-validate on boot
- Config backup and restore: `GET /api/config/export` (key-protected when a key is set, WiFi credentials excluded) and `POST /api/config/import`
- Optional WireGuard client: vendored `esp_wireguard` component ported to ESP-IDF 6, configured via the Network tab or `GET/POST /api/wireguard`, status in `/api/status`
- Brightness cap as a power-budget guard: requests above the cap are clamped, `POST /api/display/brightness_cap`
- Factory reset endpoint `POST /api/system/factory_reset`: wipes display settings, keeps WiFi credentials
- Device name, reported in webhook payloads
- Host-side unit tests for the pure modules (`mapping.c`, `valuefmt.c`), plain gcc and assert, no ESP-IDF needed

### Changed

- Breaking: the partition table switches from a single factory app to two 6 MB OTA slots. Installing 2.0.0 requires one USB reflash; the `nvs` partition keeps its v1 offset, so stored settings and WiFi credentials survive. All later updates go over the web UI or the OTA API
- Breaking (build): `web_ui.h` (embedded C string) is removed; the web UI is real source files under `Firmware/main/web/`, gzipped at build time and embedded via `EMBED_FILES`
- The compile-time mapping enum is replaced by the runtime layout config. A stored v1 `map_mode` migrates automatically on first boot; `POST /api/display/map` remains as a compatibility shim that installs the equivalent preset layout
- The HTTP layer splits into per-domain handler files (`api_display.c`, `api_layout.c`, `api_system.c`, `api_wifi.c`, `api_colors.c`, `api_config.c`, `api_integrations.c`, `api_ota.c`), with `web_server.c` reduced to server setup, static assets and captive-portal probes
- Frame and scroll buffers move from static arrays to heap allocations sized from the active layout

## [0.1.0]

The v1 baseline, never tagged as a release. WiFi station mode with a captive-portal setup AP, text display with a 5x7 font and integer scaling, colour, brightness, scrolling, seven compile-time mapping presets for the panel wiring, test patterns, per-weekday on/off schedule with SNTP time sync, and settings persistence in NVS.
