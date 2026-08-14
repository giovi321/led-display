# Changelog

All notable changes to the firmware are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
