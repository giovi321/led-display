---
title: Firmware architecture
description: Module map, boot order, web asset pipeline, and partition table of firmware v2
sidebar:
  order: 1
---

Firmware v2 (current version 2.12.0, `fw_version.h`) is a single ESP-IDF application targeting the ESP32-S3, built with ESP-IDF v6.0.1. All application code lives in `Firmware/main/`; the only extra component is the vendored WireGuard client in `Firmware/components/esp_wireguard/`. Modules with pure logic (pixel mapping, value formatting, the fonts, the saved-network ordering, the device-name rules, the light-sensor curve) contain no ESP-IDF calls so they compile and run on the host, see [building](../../development/building/).

![Firmware module map](/led-display/assets/diagrams/firmware-modules.svg)

## Boot order

`main.c` runs the following sequence, in this order:

1. Initialize NVS, erasing and retrying on version mismatch or full pages
2. Load settings from the `cfg` NVS namespace (`settings_load`), defaults on first boot
3. Resolve the layout: stored `layout` blob, else a one-time migration of the legacy v1 `map` key via `mapping_from_legacy` (persisted so the legacy key is never consulted again), else the factory default
4. Bring up the LED stack: `led_driver_init` with the resolved layout, then the power budget and brightness cap before brightness (a value over either never reaches the panels), then `display_init` and the stored font and spacing
5. Boot splash: a 1 s border in the stored color, visual confirmation the LEDs are alive
6. Start WiFi (`wifi_manager_start`): set the hostname on both netifs before the radio starts so the first DHCP request carries it, then scan and walk the saved networks strongest-first with a 10 s timeout each, falling back to the captive-portal AP. Once boot ends in STA mode a reconnect task keeps retrying a lost link
7. Load integration config, start `value_feed` (its fallback clock starts here, so "no value since boot" counts from now) and the outbound webhook worker; all non-fatal, a failure logs a warning and boot continues
8. Show the configured boot screen: stored text, the IP banner, or blank (first boot with no stored text falls back to the IP banner). In AP mode the scrolling setup banner overrides the choice, and a `wifi_manager` state callback re-shows it if the AP is raised later
9. Start the scheduler: SNTP sync, on/off schedule, and the 30 s tick that evaluates the value fallback
10. Initialize the ambient light sensor, after the scheduler on purpose: a first reading re-applies the brightness through the scheduler's arbiter, which needs its mutex to exist
11. Start the HTTP server, so endpoints become reachable only once display state is settled
12. Call `ota_mark_valid`. The web server answering is the "provably up" bar; a crash before this point rolls back to the previous OTA slot on the next boot
13. Start the outbound clients, WireGuard, MQTT and the URL value poller, all best effort: runtime-disabled until configured, no-op stubs when compiled out for the first two. They come last because they dial out to networks this image has never met, and a fault here before step 12 would roll back an otherwise good update
14. Fire the boot webhook

## Module map

### Display stack

`mapping.c` is the pixel-mapping engine: pure C11, no ESP-IDF, no allocation. It turns a layout (`mapping_config_t`) into a lookup table from logical coordinates to chain positions. Details in [pixel mapping](../pixel-mapping/).

`led_driver.c` owns the WS2812B chain: the `espressif/led_strip` managed component over RMT (GRB, GPIO 16, 10 MHz RMT resolution, no DMA), a heap frame buffer, and the LUT built once by `mapping_build_lut`. Brightness is applied as a scale factor on refresh, clamped by a separate brightness cap and, when the limiter is enabled, by the calculated power budget (see [power](../power/)). Refresh runs a read-only summing pass over the frame buffer first, which is what prices the frame: brightness scaling is uniform, so one raw sum is enough to cost any brightness, and `power_model.c` solves the same formula backwards for the highest brightness that fits the budget. `led_driver_reconfigure` swaps the layout at runtime: validate, reallocate frame buffer and LUT, resize the strip, clear. An invalid or missing layout at boot falls back to the factory default instead of bricking the device.

`display.c` renders on top of the driver: text at an integer scale factor, static or scrolling in restart or continuous mode (FreeRTOS task), calibration test patterns (raw strip walk, logical (x,y) walk, corners, border, fill), and the identify mode that lights each panel in a color keyed by its chain index for the arrangement editor.

`fonts.c` holds the glyph data and the spacing rules, pure C: three column-major ASCII fonts (`5x7`, `3x5`, `7x12`), proportional advance with tabular digits, monospace as the alternative, and `font_fit`, which substitutes the tallest font that fits when the stored one is too tall for the active layout.

`led_driver_freeze()` pushes one frame at a fixed brightness and then stops transmitting until thawed. Both OTA paths call it before their first flash operation: the RMT interrupt is not IRAM-safe, so a flash write stalls it mid-frame and the strip re-latches out of alignment, scrambling the panel and spiking the current (see [power](../power/)). The frame buffer is untouched, so a failed update thaws and repaints; a successful one reboots with the frame still latched.

`power_model.c` is the WS2812B current model and nothing else: per-LED quiescent draw plus a share of full-scale channel current, integer-only, no ESP-IDF, host-tested. It is called once per refresh by `led_driver.c` and mirrored in `app.js` so the web UI can price a slider move without a round trip.

`light_sensor.c` reads a BH1750 lux meter over I2C at address 0x23 once a second and hands the level to the scheduler's brightness arbiter; `light_curve.c` is its pure half (reading to brightness, smoothing, hysteresis, pin rules). Lux is clamped onto a 0..4095 scale, which is already brighter than any readable room. Sampling runs whenever the sensor answers, so the UI can calibrate against a live reading before it is switched on, and a sensor that stops answering is reported rather than frozen.

### Settings and NVS

`settings.c` persists everything under the NVS namespace `cfg`. Scalar values (text, color, brightness, brightness cap, power budget and its two calibration constants, scroll mode, speed, font, spacing, font scale, boot screen, device name) have individual keys with read-modify-write helpers per field, so HTTP handlers never need the full schema. The `scrl` on/off bool is kept in sync with the `scm` scroll mode so a rollback to an older image still finds a value it understands. Structured config is stored as JSON blobs (up to 2048 bytes each) in the same namespace:

| Blob key | Content |
| --- | --- |
| `layout` | panel type, grid size, per-cell chain index and rotation |
| `colors` | saved color presets |
| `valuecfg` | value formatting and fallback config (blob version 2; a version 1 blob with the old stale settings is migrated on first load) |
| `vpoll` | URL poll config: enabled, URL, interval, value path, custom header |
| `integr` | API key, outbound webhook config (URL, events, custom header, heartbeat interval), and the device-name copy the `dname` key superseded |
| `wg` | WireGuard client config |
| `mqtt` | MQTT broker URI, credentials, base topic, Home Assistant discovery |
| `lsen` | light sensor I2C pins, calibration readings, brightness range, curve |

The legacy v1 `map` key (integer mapping mode 0..6) is read once and migrated to a `layout` blob at boot.

Two namespaces sit outside `cfg`: `sched` for the schedule scalars plus its off-window action and dim level, and `wifi` for the five credential slots `ssid0`..`ssid4` / `pass0`..`pass4`. Slot 0 is mirrored onto the single legacy `ssid`/`pass` pair, and a device that only has that pair adopts it into slot 0 on the first boot. Because the WiFi and schedule namespaces are separate, `POST /api/system/factory_reset` (which erases `cfg`) leaves both alone.

### HTTP API

`web_server.c` starts `esp_http_server` on port 80 (wildcard URI matching, 72 handler slots) and calls one registration function per domain module. Each module owns its endpoints exclusively; the ownership table lives in `api_handlers.h`:

| File | Endpoints |
| --- | --- |
| `api_display.c` | `/api/display/{text,brightness,brightness_cap,power,light_sensor,clear,test,map,identify}` |
| `api_layout.c` | `/api/layout` (GET/POST) |
| `api_colors.c` | `/api/colors`, `/api/value`, `/api/value/config`, `/api/value/preview` |
| `api_value_poll.c` | `/api/value/poll` (GET/POST), `/api/value/poll/now` |
| `api_wifi.c` | `/api/wifi/{scan,networks,networks/delete,connect,reset}` |
| `api_system.c` | `/api/status`, `/api/system/{reboot,boot_screen,device_name,factory_reset}`, `/api/schedule` |
| `api_config.c` | `/api/config/{export,import}` |
| `api_ota.c` | `/api/ota/{upload,url,status}` |
| `api_integrations.c` | `/api/integrations`, `/api/wireguard`, `/api/mqtt` |
| `web_static.c` | `/`, gzipped assets, captive-portal probes |

`web_common.c` provides the shared helpers: JSON request/response plumbing and authentication. Auth is a single `X-Api-Key` header compared in constant time against the stored key; an empty stored key disables auth. Mutating handlers call `web_require_auth` first and get a 401 JSON error sent for them on mismatch.

### WiFi

`wifi_manager.c` owns the radio: the five saved-network slots and their NVS mirroring, the connect passes, the captive-portal AP with its DNS hijack, and the reconnect task. `wifi_pick.c` is the pure half that decides the order of a connect pass: every saved SSID the scan saw, strongest first (duplicate hits for one SSID count as their strongest, ties keep stored order), then the ones the scan missed in stored order so a hidden SSID still gets an attempt. A radio mutex serialises API scans against a running connect pass. State changes are reported to `main.c` through a callback, which is what puts the setup banner on the panels and takes it off again.

### Scheduler

`scheduler.c` syncs wall time over SNTP and drives two periodic jobs on a 30 s tick: the per-day display on/off schedule (day bitmask, on/off times, UTC offset in minutes, and whether the off window blanks the panel or holds it at a dim level) and a call to `value_feed_tick()`, which evaluates the value fallback (see below).

It also owns the one function that decides what brightness reaches the panel, so nothing else has to know the precedence. In order: the schedule level (inside the on window the stored user level, or the light sensor's level while that is enabled and has read something; outside it 0 or the dim level), then the brightness cap, then the fallback dim, which halves the result. Clamping before the halving is deliberate, because a cap at or below half the level would otherwise swallow the dim entirely. In setup-AP mode the schedule and the fallback stand down and the sensor is bypassed, because the panel then carries the only copy of the AP name and address.

### Integrations and webhooks

`integrations.c` caches the integration config (API key, webhook URL, event mask, webhook auth and heartbeat interval) and the value formatting and fallback config in RAM, backed by the `integr` and `valuecfg` blobs, and tracks when the last value arrived. It also converts a version 1 `valuecfg` blob (the 2.11.0 stale settings and placeholder) to version 2 on first load, through `fb_migrate_legacy`. `valuefmt.c` is the pure formatting half: raw value string to display text via multiplier, scale, decimals, separators, plus sign, prefix and suffix, host-testable like the mapping engine.

Every value source ends in one place. `value_feed.c` is the ingest path for `POST /api/value`, MQTT `value/set` and the URL poller: it formats the raw value, stores it as the last value, records the arrival, clears the fallback and renders the panel when the display source is integration. It also owns the fallback state, feeds it poll results and MQTT connect state, and fires `value_stale` and `value_recovered` and the MQTT state publish only after releasing its mutex. `fallback.c` is the pure state machine behind it: triggers are events, not conditions re-checked each tick, and every call takes the time as an argument, so it is host-testable with a fake clock. The scheduler's brightness arbiter reads the resulting dim flag without taking the feed's mutex.

`value_poll.c` is the URL poller: one low-priority task that fetches the configured URL on its interval, with a 10 s socket timeout and a whole-request deadline, a 4 KB body cap, redirects followed by hand and TLS checked against the IDF certificate bundle. `jsonpath.c` is the pure scanner that pulls one string or number out of the body by a dotted path with `[n]` indices, without building a tree and without heap, so it is host-tested like `valuefmt.c`. The poller never runs on the HTTP server or display task, and `POST /api/value/poll/now` only wakes it.

`devname.c` is the pure device-name module: the charset rules, a lenient cleaner for values that were never validated against them (an older webhook name, an imported config file), and the resolvers that turn a stored name into a hostname or a setup-AP SSID, defaults included. `integrations.c` reads the stored name through it and adopts a customised pre-2.2 webhook name once; `wifi_manager.c` applies it to both netifs and to the AP config.

`webhooks.c` posts JSON events (boot, IP change, OTA result, schedule on/off, value stale, value recovered) to one configured URL from a low-priority worker task: fire and forget, one retry, 5 s timeout, with one optional custom header. The same task owns the status heartbeat: it waits on its event queue with a timeout equal to the time left until the next heartbeat and posts `status` when that runs out, whichever events are selected.

### OTA

`ota_update.c` feeds two paths into the same `esp_ota` machinery: the web upload streams the request body chunk by chunk, and the URL pull runs `esp_https_ota` in a worker task (HTTP allowed for LAN hosts, TLS with the IDF certificate bundle otherwise). Rollback is armed by `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: a new image that never reaches `ota_mark_valid` reverts on the next boot.

### MQTT

`mqtt_link.c` wraps `esp-mqtt` behind the same shape as the WireGuard client: compiled in when `CONFIG_LEDDISP_MQTT=y` (default on), no-op stubs otherwise, and down until a broker URI is configured. It subscribes the `set` topics (text, value, brightness, power, source, reboot), publishes a retained state document every 30 seconds and after each command, and registers a last will so the broker marks the device offline on an unclean drop. Values arriving on `value/set` go through `value_feed` like `POST /api/value`, so they share its formatting and fallback bookkeeping. The state document carries the fallback flag and reason, and the connect and disconnect events feed the fallback's MQTT-loss trigger. Home Assistant discovery, when enabled, publishes retained config topics on every connect and clears them with empty retained payloads when switched off.

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
