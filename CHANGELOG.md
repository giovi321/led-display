# Changelog

All notable changes to the firmware are documented here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [2.12.0] - 2026-10-28

### Added

- The device can fetch the value itself. A new Value from URL card polls an http or https URL on an interval (10 to 86400 seconds, default 300), so a source that cannot push into the network can still drive the display. A value path (`data.price`, `[0].value`, `data.items[2].value`) picks the field out of a JSON response, and an empty path takes the whole trimmed body, shown exactly as received when it is not a number. An optional secret is sent as `Authorization: Bearer`, and one custom header can be set next to it. HTTPS is verified against the certificate bundle, the request times out after 10 seconds, and the body is capped at 4 KB. Polling runs on its own low-priority task, so a slow URL never blocks the web server, the display or MQTT. `jsonpath.[hc]` is the pure scanner behind the path, host-tested
- `GET`/`POST /api/value/poll` and `POST /api/value/poll/now`. The GET reports the config and the status of the last poll (time, HTTP status, extracted value, error, consecutive failures), and the secret and header value only as `secret_set` and `hdr_value_set`. The POST keeps stored secrets unless `clear_secret` or `clear_header` is sent. "Poll now" does not wait for the result: it returns `pending` at once, and `pending` is false when polling cannot run. Saving the config triggers one poll. Header names must be valid HTTP tokens and values must not contain line breaks. The poll settings travel in the config backup as a `value_poll` section
- More formatting for every value source: a multiplier, scaling to thousands, millions or billions (or `auto`), configurable scale letters, a decimal comma next to the existing separators, and an optional plus sign. A rounding that crosses a unit moves up with it, so 999.96k at one decimal shows 1.0M. `POST /api/value/preview` formats a sample value with a posted, unsaved config and returns the text, and the new Formatting card uses it for a live preview, so the browser carries no copy of the formatter
- One fallback for every failure, with a text of up to 64 characters and a mode of text, corner marker or dim. It engages when no value arrived for a set number of minutes, after a set number of consecutive failed polls (default 3), or when the MQTT broker is lost for more than 60 seconds (off by default). Triggers are events, not conditions re-checked each tick, so a single slow poll does not flap it and a failing poll does not switch it back on while MQTT delivers values. The next value from any source clears it. Before the first value since boot the panel shows the fallback text whatever the mode, and notifies nobody
- The status webhook gains a `value_recovered` event, an optional secret and custom header on every outbound POST, and a heartbeat. With a heartbeat interval of 1 to 1440 minutes the device posts a `status` event on that interval, whichever events are selected, with the IP, signal, uptime, firmware version, brightness, power, content source, what the panel shows, the value age, and the fallback state. `value_stale` now carries `reason` (`timeout`, `poll_failures` or `mqtt_lost`) next to `age_s`, which is -1 when no value has arrived since boot
- MQTT state gains `fallback` and `fallback_reason`, and Home Assistant discovery publishes a diagnostic "Value fallback" binary sensor that is withdrawn with the rest when discovery is turned off

### Changed

- The Integrations tab follows the data flow: Value from URL, Value endpoint, MQTT, Formatting, Fallback, Status webhook, API key. Formatting moved out of the value endpoint card because it applies to every source. Every card that holds a secret uses a password field with "leave blank to keep current"
- The fallback replaces the stale settings and the placeholder text. On the first boot of 2.12.0 the stored settings are converted: the fallback text is the old placeholder if one was set, else the old stale text, the timeout is the old stale hours times 60 minutes, and the stale action text, marker or dim becomes the fallback mode (action none becomes a timeout of 0 and mode text). A configuration that used a dot as thousands separator gets a decimal comma. A backup from 2.11.0 imports through the same conversion
- `GET /api/value/config` still returns `stale_hours`, `stale_action`, `stale_text` and `default_text` for one release, derived from the fallback settings, and `POST /api/value/config` still accepts them. Use `fb_text`, `fb_mode`, `fb_after_min`, `fb_poll_failures` and `fb_on_mqtt_loss` in new scripts
- The fallback timeout counts from boot while no value has arrived, where the old staleness guard armed only after the first value. The checks and the notifications run whatever the display source, while the panel itself changes only in integration mode
- Every value source, `POST /api/value`, MQTT and the poller, now enters through one ingest path (`value_feed.c`) and the fallback state machine is `fallback.[hc]`, host-tested with a fake clock. The valuecfg blob is version 2, and the decimals range is -1 to 6 where it was -1 to 9
- JSON numbers posted to `/api/value` are kept to 15 significant digits where they were cut to 10
- `GET /api/status` and the MQTT state report `value_age_s` as the time since the last value from any source
- The API key card moved last and says it gates every POST, the value, preview and poll endpoints included

## [2.11.0] - 2026-09-01

### Added

- A calculated power budget. The driver adds up the colour values in every frame before it goes out and prices them against a model of the panels, so the current an arrangement will draw is known rather than guessed. `power_model.[hc]` is that model: integer-only, no ESP-IDF, host-tested
- The Appearance tab prices the brightness slider live. It shows the estimated draw of the content actually on the panel at the slider's position with a bar against the budget, what every pixel lit at that colour would cost, and the fixed floor the chain draws with everything black. Crossing the budget turns the figure red, opens a warning naming both numbers and the consequence, and makes Apply ask a second time
- An opt-in power limiter under Hardware. With it on, a frame over budget goes out at the highest brightness that fits instead of tripping the supply. The clamp is per frame and presentation only: the stored brightness is untouched and comes back on its own when the content or the colour costs less. Off by default, so an existing display behaves exactly as before after the update
- `POST /api/display/power` sets the budget, the limiter and the two calibration constants (current per colour channel at full, and per LED when black). `GET /api/status` gains a `power` block with the live estimate, a two-second peak hold, the lit-pixel count and whether the limiter is clamping. The constants and the budget travel in the config backup's display section

### Fixed

- A firmware update no longer scrambles the panel or cuts the supply out. The strip is driven from a non-DMA RMT channel whose interrupt is not IRAM-safe, so it cannot refill its 64-symbol buffer while a flash erase or write has the cache disabled. A stall longer than the WS2812B's ~50 us reset window makes the strip latch and restart the frame mid-stream, so the remaining bits land at the wrong LED and at the wrong bit offset inside each byte - and misalignment turns small channel values into large ones, which is both the garbage on the panel and a current spike far above what the content should draw. `OTA_WITH_SEQUENTIAL_WRITES` erases throughout the transfer rather than once at the start, and nothing stopped the refresh loop, so this repeated for the whole update; a continuous scroll at high speed transmits almost without gaps and hits it every time
- Both update paths now freeze the strip before the first flash operation: one static frame goes out at a fixed low brightness and then nothing at all, because a frame nobody retransmits cannot be torn. A frame that is already dark is also immune to the corruption itself - misaligned zero bits are still zero - which is why this is stronger than simply lowering the brightness. The panel shows a short notice, or goes dark on a display too narrow for one
- The frozen frame is deliberately left latched through the reboot: the LEDs keep their own power across a restart, so the notice stays until the new image clears the strip, instead of bright garbage surviving into the next boot. A failed update (rejected image, dropped connection, no OTA slot) thaws and repaints instead

### Changed

- The brightness cap's help text now says what it is next to the budget: a ceiling that scales colour without knowing what the frame will draw. It is unchanged and still applies first
- The power budget page documents the model, both defaults and why the per-channel default is the datasheet figure rather than the one fitted to the project's own measurement, plus how to calibrate against a meter

## [2.10.0] - 2026-08-15

### Removed

- The analog light sensor. The BH1750 added in 2.9.0 is now the only source, and the ADC path, the `type` selector, the `gpio` field and `valid_gpios` are gone with it. 2.9.0 kept both for one release; this drops the one the board has no connector for
- A stored analog configuration is discarded rather than migrated, and the sensor comes up switched off, so the display returns to your saved brightness instead of to a reading it cannot take. The calibration points could not be carried over: they are ADC counts from one particular divider, and nothing turns those into lux. Fit a BH1750 and run the two-point calibration again
- A backup taken before this release has the same problem and its `light_sensor` section is reported as skipped on restore rather than applied

### Changed

- The `lsen` config blob is v3: `enabled`, `sda`, `scl` and the curve. A v1 or v2 blob is refused on load for the reason above
- The Hardware tab's Light sensor card loses the sensor selector and the ADC pin row, leaving SDA, SCL and the same two-point calibration. The live reading is labelled in lux

## [2.9.0] - 2026-08-15

### Added

- Support for the BH1750 digital lux meter as a light-sensor source, alongside the analog input the firmware has always had. Neither is preferred and both stay first-class: the analog divider needs no extra module and works on a v3 board, the BH1750 reports real lux and plugs into a connector. Pick one under Sensor on the Hardware tab, or send `type` to `POST /api/display/light_sensor`
- Everything after the reading is shared between them: the same two calibration points, the same response curve, smoothing, hysteresis and handover to the brightness arbiter. Only three small functions in `light_sensor.c` know which sensor is fitted, so the calibration flow is identical either way. Switching source discards the smoothed reading, because the next sample comes from different hardware on a different scale
- `GET /api/display/light_sensor` reports `type`, both pin sets and `valid_i2c_gpios`, so a client never needs its own copy of which pins the module can spare. `sampling` now means "configured **and** answering", so a BH1750 that is unplugged or miswired says so instead of showing a frozen reading

### Changed

- The controller board is revision v4: it adds a 4-way `GND SDA SCL 5V` connector for the sensor, taking the board from six connectors to seven and from 90 drilled holes to 94. Nothing else moved. SDA reaches GPIO8 and SCL GPIO9, which is what the firmware defaults to
- The `lsen` config blob is v2, adding `type`, `sda` and `scl`. A v1 blob has none of those and loads as analog, which is the only thing it could have been, so an update changes no behaviour. The same applies to a backup taken before this release
- I2C pins are validated against what the ESP32-S3-N16R8 can actually spare: not the strapping pins 0, 3, 45 and 46, not GPIO22 to GPIO25 which the chip does not bring out, and not GPIO26 to GPIO37, which carry the module's own flash and PSRAM. Taking one of those last two out does not misconfigure the sensor, it stops the module booting
- The board documentation says JST-XH 2.54 mm for all seven connectors, which is what is fitted. The previous wording described the footprints as generic headers, which is what the design file draws but not what goes in them

## [2.8.1] - 2026-08-15

### Changed

- The Web UI login card moves from the Integrations tab to the System tab. It governs the whole interface rather than one integration, so it belongs with the other device-wide settings. Nothing about the login itself changes, and the card loads its own state, so it works whichever tab you land on

## [2.8.0] - 2026-08-15

### Added

- An optional login for the web UI, off by default and off after an update. Tick the box on the Integrations tab, set a username and password, and the browser asks for them before it will show anything. It covers every URI the device serves - pages, assets and the whole API, the open GETs included - because a login that leaves the configuration readable is not one. The API key is unchanged and still gates writes for scripts; the two compose. `ui_auth`, `ui_user` and `ui_pass` on `POST /api/integrations`, with `ui_auth` without a stored password refused rather than silently ignored
- Two limits worth knowing before turning it on. It is HTTP Basic over plain HTTP, so it keeps casual visitors on your network out and does nothing against anyone who can watch the traffic. And it is not applied on the setup AP, where the panel carries the only copy of the setup address and the captive-portal probes have to answer normally - which is also the way back in if the password is lost, since the board has no reset button
- The project icon is the web UI's favicon, so the tab is identifiable among a row of them. Served from the device at `/favicon.svg`, and at `/favicon.ico` too, since that is what a browser asks for unprompted
- A choice for what an unticked day does, next to Active days. Until now a day outside the mask was one long off-window, so ticking only Monday left the panel dark for six days - correct if you wanted "lit on weekdays, dark all weekend", surprising if you meant "the schedule only governs Monday". Both are now sayable: "Apply the off-action all day" keeps the old behaviour and is the default, "Run normally, ignore the schedule" hands those days back to the normal brightness. `other_days` on `GET`/`POST /api/schedule` and in the backup, `off` or `run`; absent anywhere it stays `off`, so no device changes behaviour on update

### Changed

- The schedule's window decision moves into `sched_window.c`, free of NVS, SNTP and the LED driver, and gains host tests: the day-mask conversion, both edges of the window, the midnight wrap, the wrap not carrying into an unticked day, and each unticked-day rule. It is the part that is easy to get wrong and it had no coverage at all
- The build helper runs the host tests by default; `TESTS=0` skips them. The old opt-in `TESTS=1` was being dropped somewhere between the Windows shell and WSL, so the suite had been quietly not running, and it now says so when it skips

## [2.7.0] - 2026-08-15

### Fixed

- Updating from a GitHub release always failed at the download with "download failed to start: ESP_FAIL", while uploading the same `.bin` by hand worked. The asset URL is short, but GitHub answers it with a redirect to a signed URL whose query string alone runs to about 800 bytes, and the HTTP client formats the whole request line into a transmit buffer that was sized for the 512-character URL the API accepts, not for where that URL leads. It now gets 2 KB, with room for GitHub to lengthen those signatures again

### Changed

- The single-pixel offline indicator is an overlay rather than a screen. It used to blank the panel and light one pixel, which threw away the content it was meant to annotate; it now leaves the display exactly as it was and lights one pixel on top. Its position is a free coordinate instead of a corner, and it has its own brightness, applied instead of the panel brightness and its cap, so it stays visible on a panel dimmed by the light sensor or turned off by the schedule. Coming back online takes the pixel away and repaints nothing
- `offline_corner` is replaced by `offline_x`, `offline_y` and `offline_brightness` on `POST /api/system/screens` and in `GET /api/status`. A stored corner is converted to coordinates on the first boot, and a backup taken before this release imports through the same conversion
- The drawing of the reference build is redrawn from the cut file rather than around it. All twelve fixings are the M5 holes the DXF actually contains, at one size, including the four near the corners that were missing; the four oversized screws previously drawn outside them were the corner fillets of the sheet outline, not holes. The heads are drawn at the 10 mm of a countersunk M5 rather than at the diameter of the hole beneath them, and with a hex socket rather than a slot
- The section is to scale. The layers sit at their real thicknesses, so the 6 mm short spacers and the 22 mm long ones are visibly different gaps instead of five evenly spaced bands, and the spacers themselves are drawn as the parts they are. The screws are now visible as screws: countersunk head buried in the front panel, shank through the stack, dome nut closed on the back
- The assembly guide calls the closing nuts acorn nuts, not "acorn cap nuts". Acorn, cap and dome are three names for the same DIN 1587 fastener, so the pair was a tautology

## [2.6.0] - 2026-08-15

### Fixed

- Disabling the schedule from inside an off-window left the panel dark. The off-window flag is what the brightness arbiter reads, and it was only ever cleared by the 30-second tick, so until that came round the display stayed at zero and every brightness change made in the meantime was stored and then thrown away on its way to the panel. Both the tick and a schedule save now re-evaluate the window immediately, in either direction

### Removed

- The scroll trail added in 2.5.0. It rendered badly and has been taken out rather than tuned: `fade` on `POST /api/display/text` is gone, and the stored value is ignored

### Changed

- The render of the reference build now carries the twelve perimeter fixings from the drawings, in both the front view and the section, and is drawn to the correct 670 by 190 mm sheet with its 5.25 mm corner radius. The section labels no longer overlap and the stray dimension beside it is gone
- The render also appears on the documentation landing page and the overview, and the first web UI screenshots are in the README, the display guide, the arrangement editor guide, the MQTT guide and the WireGuard guide

## [2.5.2] - 2026-08-15

### Fixed

- Pressing Save on the WireGuard card while the form was still empty wrote that emptiness over a working tunnel: the card sends the whole form, so an unloaded page silently cleared the endpoint, address and allowed IPs, leaving only the private key. Saving an entirely blank form is now refused, with Reset config as the deliberate way to clear it

## [2.5.1] - 2026-08-15

### Fixed

- Choosing the single-pixel offline indicator saved correctly but read back as "keep showing the last content", so the web UI showed the wrong option and reverted the choice on the next save. Three range checks had been left at the old end of their enums when the options were added: `offline_screen` in the status and in the config import, and `boot_screen` when loaded from flash, which would have discarded a stored `ip_then_content` on the next boot

## [2.5.0] - 2026-08-15

### Added

- Every tab has its own address: `/display`, `/schedule`, `/hardware`, `/network`, `/integrations`, `/system`. A reload, a bookmark or the back button now land where you were, and a link to an advanced tab turns advanced mode on rather than silently showing something else
- Trail, on the Display tab: how much of each frame is left behind as text moves, instead of the pixels going out at once. At 0 nothing changes; above that a scrolling value drags a decaying smear behind it the way a slow phosphor does. `fade` on `POST /api/display/text`
- A lost WiFi link can now light a single pixel, in a colour and corner you choose, alongside the existing keep, message and blank. It is the quietest way for a wall-mounted display to admit it is offline: visible if you look, invisible from across the room
- After boot the display can show its address for five seconds and then switch to the content, instead of choosing one or the other permanently. `ip_then_content` on `POST /api/system/boot_screen`
- Reset button for the WireGuard configuration, and `{"reset": true}` on `POST /api/wireguard`. Every field there keeps its stored value when omitted, and the private key cannot be cleared with an empty string, so there was no way back to blank
- A link to the project on GitHub in the web UI footer
- A drawing of the reference build in the README and the hardware guide, generated from the cut sizes in `3D/8x32/` and the firmware's own font, so the lit pixels are the ones the device would light

### Changed

- Without WiFi the panel scrolls `AP mode, SSID: ..., IP: ...` instead of `Setup AP: ... / ...`, saying what it is rather than abbreviating it
- The WireGuard key fields dropped the "put this ON THE SERVER" and "copy this FROM the server" suffixes from their labels; the explanation was already a tooltip away
- README opens with the render and a shorter description

## [2.4.1] - 2026-08-15

### Changed

- The WireGuard card no longer leads with this device's public key. Most people never need it: a server that generates the client configuration for you - UniFi, and most router and VPN provider interfaces - derives the public key itself and only ever hands you a private key, so a prominent field for it invites the question of what it is meant to pair with. It has moved into a "Connection details" section together with the endpoint address, the tunnel interface as lwIP built it, and the count of packets that have come down the tunnel: present when something needs diagnosing, out of the way when the tunnel simply works

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
