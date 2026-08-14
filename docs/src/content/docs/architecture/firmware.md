---
title: Firmware architecture
description: Module map, boot order, web asset pipeline, and partition table of firmware v2
sidebar:
  order: 1
---

Firmware v2 (version 2.0.0, `fw_version.h`) is a single ESP-IDF application targeting the ESP32-S3, built with ESP-IDF v6.0.1. All application code lives in `Firmware/main/`; the only extra component is the vendored WireGuard client in `Firmware/components/esp_wireguard/`. Modules with pure logic (pixel mapping, value formatting) contain no ESP-IDF calls so they compile and run on the host, see [building](../../development/building/).

![Firmware module map](/led-display/assets/diagrams/firmware-modules.svg)

## Boot order

`main.c` runs the following sequence, in this order:

1. Initialize NVS, erasing and retrying on version mismatch or full pages
2. Load settings from the `cfg` NVS namespace (`settings_load`), defaults on first boot
3. Resolve the layout: stored `layout` blob, else a one-time migration of the legacy v1 `map` key via `mapping_from_legacy` (persisted so the legacy key is never consulted again), else the factory default
4. Bring up the LED stack: `led_driver_init` with the resolved layout, then brightness cap before brightness (an over-cap value never reaches the panels), then `display_init`
5. Boot splash: a 1 s border in the stored color, visual confirmation the LEDs are alive
6. Start WiFi (`wifi_manager_start`): STA with stored credentials and a connect timeout, falling back to the captive-portal AP
7. Load integration config and start the outbound webhook worker; both are non-fatal, a failure logs a warning and boot continues
8. Show the configured boot screen: stored text, the IP banner, or blank (first boot with no stored text falls back to the IP banner)
9. Start the scheduler: SNTP sync, on/off schedule, value staleness ticks
10. Initialize the WireGuard client, best effort: runtime-disabled until configured, a no-op stub when compiled out
11. Start the HTTP server, last, so endpoints become reachable only once display state is settled
12. Call `ota_mark_valid` (the web server answering is the "provably up" bar; a crash before this point rolls back to the previous OTA slot on the next boot), then fire the boot webhook

## Module map

### Display stack

`mapping.c` is the pixel-mapping engine: pure C11, no ESP-IDF, no allocation. It turns a layout (`mapping_config_t`) into a lookup table from logical coordinates to chain positions. Details in [pixel mapping](../pixel-mapping/).

`led_driver.c` owns the WS2812B chain: the `espressif/led_strip` managed component over RMT (GRB, GPIO 16, 10 MHz RMT resolution, no DMA), a heap frame buffer, and the LUT built once by `mapping_build_lut`. Brightness is applied as a scale factor on refresh, clamped by a separate brightness cap (the power-budget guard, see [power](../power/)). `led_driver_reconfigure` swaps the layout at runtime: validate, reallocate frame buffer and LUT, resize the strip, clear. An invalid or missing layout at boot falls back to the factory default instead of bricking the device.

`display.c` renders on top of the driver: a built-in 5x7 ASCII font with an integer scale factor, static or scrolling text (FreeRTOS task), calibration test patterns (raw strip walk, logical (x,y) walk, corners, border, fill), and the identify mode that lights each panel in a color keyed by its chain index for the arrangement editor.

### Settings and NVS

`settings.c` persists everything under the NVS namespace `cfg`. Scalar values (text, color, brightness, brightness cap, scroll, speed, font scale, boot screen) have individual keys with read-modify-write helpers per field, so HTTP handlers never need the full schema. Structured config is stored as JSON blobs (up to 2048 bytes each) in the same namespace:

| Blob key | Content |
| --- | --- |
| `layout` | panel type, grid size, per-cell chain index and rotation |
| `colors` | saved color presets |
| `valuecfg` | value endpoint formatting and staleness config |
| `integr` | API key, outbound webhook config, device name |
| `wg` | WireGuard client config |

The legacy v1 `map` key (integer mapping mode 0..6) is read once and migrated to a `layout` blob at boot.

### HTTP API

`web_server.c` starts `esp_http_server` on port 80 (wildcard URI matching, 48 handler slots) and calls one registration function per domain module. Each module owns its endpoints exclusively; the ownership table lives in `api_handlers.h`:

| File | Endpoints |
| --- | --- |
| `api_display.c` | `/api/display/{text,brightness,clear,test,map,identify}` |
| `api_layout.c` | `/api/layout` (GET/POST) |
| `api_colors.c` | `/api/colors`, `/api/value`, `/api/value/config` |
| `api_wifi.c` | `/api/wifi/{scan,connect,reset}` |
| `api_system.c` | `/api/status`, `/api/system/{reboot,boot_screen}`, `/api/schedule` |
| `api_config.c` | `/api/config/{export,import}` |
| `api_ota.c` | `/api/ota/{upload,url,status}` |
| `api_integrations.c` | `/api/integrations`, `/api/wireguard` |
| `web_static.c` | `/`, gzipped assets, captive-portal probes |

`web_common.c` provides the shared helpers: JSON request/response plumbing and authentication. Auth is a single `X-Api-Key` header compared in constant time against the stored key; an empty stored key disables auth. Mutating handlers call `web_require_auth` first and get a 401 JSON error sent for them on mismatch.

### Scheduler

`scheduler.c` syncs wall time over SNTP and drives two periodic jobs: the per-day display on/off schedule (day bitmask, on/off times, UTC offset in minutes) and the value staleness tick, which triggers the configured stale action (dim, corner marker, or replacement text) when no value update arrived within the configured window.

### Integrations and webhooks

`integrations.c` caches the integration config (API key, webhook URL and event mask, device name) and the value formatting config in RAM, backed by the `integr` and `valuecfg` blobs, and tracks when the last value arrived. `valuefmt.c` is the pure formatting half: raw value string to display text via prefix, suffix, decimals, and thousands separator, host-testable like the mapping engine.

`webhooks.c` posts JSON events (boot, IP change, OTA result, schedule on/off, value stale) to one configured URL from a low-priority worker task: fire and forget, one retry, 5 s timeout.

### OTA

`ota_update.c` feeds two paths into the same `esp_ota` machinery: the web upload streams the request body chunk by chunk, and the URL pull runs `esp_https_ota` in a worker task (HTTP allowed for LAN hosts, TLS with the IDF certificate bundle otherwise). Rollback is armed by `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: a new image that never reaches `ota_mark_valid` reverts on the next boot.

### WireGuard

`wg_client.c` wraps the vendored `esp_wireguard` component. It is compiled in only when `CONFIG_LEDDISP_WIREGUARD=y` (default on); when compiled out, no-op stubs keep callers free of `#ifdef`s and the API reports `compiled_in=false`. The tunnel stays down until enabled and configured. All component calls are marshalled through the lwIP tcpip thread, the client waits for SNTP before connecting (handshake timestamps need sane wall time), and a 180 s handshake stall triggers a re-init to pick up DNS changes. Provenance and local changes to the component are documented in `Firmware/components/esp_wireguard/PORTING.md`.

## Web asset pipeline

The web UI is four editable source files in `Firmware/main/web/`: `index.html`, `style.css`, `app.js` (tab logic and API plumbing), and `editor.js` (the graphical arrangement editor). At CMake configure time each file is gzipped by `Firmware/tools/gzip_asset.py` into the build directory, deterministically (compresslevel 9, mtime 0) so rebuilds do not churn the binary, and embedded via `EMBED_FILES`. `CMAKE_CONFIGURE_DEPENDS` makes an asset edit re-trigger the configure step, so a plain `idf.py build` picks up UI changes.

`web_static.c` serves the embedded blobs with `Content-Encoding: gzip` and `Cache-Control: no-cache` (small files on a LAN, and a firmware update is picked up on the next page load without cache busting). It also answers the common captive-portal probe URLs (Android, iOS/macOS, Windows) with the UI page so the setup AP shows a captive sheet.

## Partition table

`Firmware/partitions.csv` targets the 16 MB flash module. `nvs` keeps its v1 offset and size, so stored settings and WiFi credentials survive the one-time USB reflash that installs this table.

| Name | Type | Subtype | Offset | Size | Purpose |
| --- | --- | --- | --- | --- | --- |
| `nvs` | data | nvs | 0x9000 | 0x6000 (24 KB) | settings, WiFi credentials (v1-compatible location) |
| `otadata` | data | ota | 0xf000 | 0x2000 (8 KB) | active-slot bookkeeping |
| `phy_init` | data | phy | 0x11000 | 0x1000 (4 KB) | RF calibration data |
| `ota_0` | app | ota_0 | 0x20000 | 0x600000 (6 MB) | app slot A |
| `ota_1` | app | ota_1 | 0x620000 | 0x600000 (6 MB) | app slot B |
| `storage` | data | nvs | 0xc20000 | 0x10000 (64 KB) | spare NVS for future use |

Two 6 MB slots leave room to grow; the current image is far smaller. How to build and flash is covered in [building](../../development/building/).
