# Changelog

All notable changes to the firmware are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.4.0] - 2026-08-15

### Added

- The header shows VPN state next to WiFi, with its own icon: connecting, up (with the tunnel address and the handshake age), or suspended. It only appears when the tunnel is switched on
- Separate settings for what the display shows **at boot**, **after boot**, and **when WiFi is lost**, replacing the single boot-screen choice. The startup splash can stay the border test (the default, because it lights every edge pixel and exposes a miswired panel in the first second), go blank, or be skipped in favour of the content. A lost WiFi link can keep the last content, show a message of your own, or blank the panel. Setup-AP mode deliberately overrides all of it, since there the panel carries the only copy of the setup address. `POST /api/system/screens`, and all three are in the config backup
- WireGuard status reports the tunnel interface as lwIP actually configured it (`netif_ip`, `netif_netmask`, `netif_up`, `netif_link_up`) and counts the inner packets that came out of the tunnel (`rx_accepted`, `rx_dropped`). A live handshake with zero accepted packets is the signature of traffic never being routed to the device, which is a question for the far end rather than the display

### Changed

- The two WireGuard key fields said "public key" in both directions and were easy to mix up. They are now "This device's public key - put this ON THE SERVER" and "Server public key - copy this FROM the server", each with a note explaining which keypair it belongs to

## [2.3.2] - 2026-08-15

### Fixed

- A tunnel that handshakes could still not be reached at its tunnel address. The tunnel address is normally written as a /32, but this device has no routing table - lwIP picks an interface by matching the destination against each interface's own subnet - so a /32 tunnel matched nothing at all, and replies to anything inside the tunnel left over WiFi instead. The tunnel interface now takes the widest Allowed IPs range that contains its own address, so `10.6.0.2/32` with allowed IPs `10.6.0.0/24` comes up as `10.6.0.2/24` and the tunnel subnet is reachable both ways. `0.0.0.0/0` is excluded, being handled by the default-interface mechanism instead

### Added

- The Network tab shows **this device's public key**, with a Copy button, and `GET /api/wireguard` reports it as `status.public_key`. Configuring the peer at the other end requires it and there was previously no way to read it off the device
- WireGuard status reports `endpoint_ip` (what the endpoint hostname resolved to), `attempts` and `stage`, so a tunnel that never connects can be told apart from one that never got as far as trying. A resolved endpoint at stage 4 with no handshake means the device is sending into the void, which points at the peer configuration or a firewall rather than at the display. The Network tab says as much in words

## [2.3.1] - 2026-08-15

### Fixed

- **WireGuard no longer panics the device when the tunnel is enabled.** ESP-IDF keeps its own per-interface handle in the lwIP netif's `state` field and its global interface-event callback casts that field back to an `esp_netif` handle unconditionally. WireGuard adds a raw lwIP interface whose `state` is the component's own device structure, so the first address event during interface creation dereferenced a pointer to the wrong type and the device rebooted - before any of WireGuard's own setup had run. ESP-IDF documents the rule in `lwipopts.h` (interfaces that use `state` themselves need the handle in netif client data) but only applies it for the bridge and PPP; the firmware now enables it explicitly with `CONFIG_ESP_NETIF_BRIDGE_EN` and `CONFIG_LWIP_NUM_NETIF_CLIENT_DATA=1`. Verified on hardware: enabling the tunnel, and rebooting with it enabled, no longer reboots the device
- A crash while bringing the tunnel up could roll back a firmware update: the tunnel and MQTT client started connecting before the new image had been marked valid, so the bootloader reverted a good update and the previous version silently reappeared. Both now start after that point
- The lwIP task stack increase shipped in 2.3.0 was not the fix it was believed to be. Measurement shows about 1.3 KB of the 8 KB stack in use at the deepest point, so it stays as headroom for the rekey path rather than as a remedy

### Added

- Timezones are now IANA zone names (`Europe/Zurich`, `America/New_York`) instead of raw POSIX rules. The picker is built from the browser's own timezone data and marks the browser's zone, and the device carries a generated name-to-rule table (418 zones, `Firmware/tools/gen_tz.js` regenerates it) so it applies daylight-saving changes itself. `GET /api/schedule` reports `tz_known` and the resolved `tz_posix`; a raw POSIX string is still accepted, so existing configurations keep working
- WireGuard bring-up diagnostics in `/api/status`: `wireguard.last_crash_stage` names the step a crash reboot died in and `wireguard.tcpip_stack_free_min` reports measured lwIP stack headroom, so a fault on the lwIP thread can be located from the API instead of guessed at. The vendored component gained an optional trace hook for the same reason (see its `PORTING.md`)
- The Network tab warns before saving `0.0.0.0/0` as Allowed IPs: a full-tunnel route carries the reply path to the browser with it, and cuts off access to the display from any computer on another subnet

## [2.3.0] - 2026-08-15

### Fixed

- WireGuard no longer crash-loops the device. Enabling the tunnel on 2.2.0 overflowed the lwIP task stack (all tunnel crypto - `psa_crypto_init` and the x25519/blake2s handshakes - runs on that thread) and every connect attempt ended in a stack-canary panic and a reboot, roughly 90 seconds after each boot, with the local network unreachable in between. The stack is now sized for it (`CONFIG_LWIP_TCPIP_TASK_STACK_SIZE=8192`)
- Crash-loop guard: three consecutive crash reboots with the tunnel enabled and the device boots with the tunnel held down instead of retrying itself to death - the web UI stays reachable, the status reports `held` and the Network tab says so. Re-saving the WireGuard config retries; a clean boot or a completed handshake resets the counter

### Added

- MQTT client (`CONFIG_LEDDISP_MQTT`, on by default, configured on the Integrations tab or `GET/POST /api/mqtt`): push values or text to the display over `<base>/value/set` / `<base>/text/set` with the same formatting as the HTTP endpoints, control brightness, power and content source, reboot, read a retained JSON state topic and an availability topic (LWT). Optional Home Assistant auto-discovery announces the display as a device with light, text, select, button and sensor entities
- Content source switch on the Display tab (`POST /api/display/source`): static text or the live integration value. The static text lives in its own card, appearance settings in another; values pushed while static is shown are remembered and appear on switch
- Placeholder text (`default_text` in the value config): shown in integration mode until the first value arrives - a rebooted display no longer resurrects a stale figure as if it were live
- Schedule timezone override: pick a named zone (daylight-saving rule stored on the device) or a custom POSIX TZ string instead of the fixed browser offset; `/api/schedule` gains `tz`
- Live panel preview in the web UI header: the current content, colour and scrolling state drawn LED by LED - not a screenshot
- Stop-tests button (Hardware tab and arrangement editor, `pattern: "off"` on `/api/display/test`): ends any test pattern or identify mode and restores the configured content
- Project logo, in the web UI header, the docs site and the README
- `/api/status` reports `display.source`, `display.text_stored`, `wireguard.held` and an `mqtt` object

### Changed

- The space thousands separator renders as a thin space, narrower than a typed space, so space-grouped values ("1 025 300") no longer overflow where dot-grouped ones fit
- The arrangement editor's grid controls are labelled steppers (Rows / Columns with joined `- value +` buttons); the wiring-path preview tile is gone
- The web UI header shows the firmware version under the title and a WiFi icon with the connection state
- `POST /api/display/text` without a `text` field no longer rewrites the stored text; `POST /api/value` no longer persists the value as the stored text (the placeholder covers reboots) and reports `rendered`
- The value-staleness guard only runs while the content source is integration, so a stale value can no longer dim or replace static text
- Docs: hardware guide and README state that the PCB expects 5 V LED panels

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
