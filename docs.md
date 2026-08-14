# Project notes

Hardware decisions, power budget, and firmware state for the LED matrix
display: a 2×2 array of WS2812B flexible 32×8 panels (1024 LEDs total)
controlled by an ESP32-S3 and powered from USB-C. Intended end-use: a dim
amber always-on readout of a single value or short message. Brightness is
intentionally low to fit the power budget.

## ESP32 choice

**ESP32-S3-DevKitC-1** (N8R2 or N16R8). Native USB for flashing, an RMT peripheral that handles WS2812B timing without burning CPU, 8 MB flash / 2 MB PSRAM as minimum spec. Around CHF 5-8. Avoid the original ESP32-WROOM for new projects — the S3's RMT is cleaner for addressable LEDs, and the native USB removes the need for an external UART adapter.

## Why a level shifter is needed

WS2812B wants 0.7 × VDD as logic-high. At 5 V supply that is 3.5 V minimum. ESP32 GPIO outputs at 3.3 V, which is marginal — some chains accept it, many don't and behave erratically. The **SN74HCT125** is a quad bus buffer that level-shifts 3.3 V logic to 5 V on a 5 V VCC rail. One channel per data line is enough; the chip has four channels available for parallel-output topologies if needed.

Pin wiring for one channel:

- Pin 1 (1OE) → GND (enables channel 1 permanently)
- Pin 2 (1A) → ESP32 **GPIO 16** (3.3 V data signal) — see firmware `LED_DATA_GPIO`
- Pin 3 (1Y) → 330 Ω resistor → Panel 1 DIN (5 V data signal)
- Pin 7 (GND) → common ground
- Pin 14 (VCC) → 5 V from VBUS
- Unused xOE pins → tie to VCC to disable

The 330 Ω in series on the data line damps reflections on the run between PCB and panel.

## Serpentine data chain

**Per-panel wiring (confirmed on the prototype, 2026-05-20).** Each flex panel is **column-serpentine, top-left origin**: DIN walks column 0 top→bottom, column 1 bottom→top, column 2 top→bottom, and so on. Earlier guesses assumed row-serpentine — the calibration "Step strip" pattern in the web UI proved otherwise.

**2×2 chain (built 2026-05-20).** All four panels are now mounted as a 2×2 grid (64×16, 1024 LEDs) wired controller → **TL → TR → BR → BL**. The bottom two panels (BR, BL) are mounted **rotated 180°** so the inter-panel jumpers stay short — from each bottom panel's own point of view its local (0, 0) is at the display's far corner of its cell. The firmware mode `LED_MAP_GRID_2X2_COL_SERP` in `Firmware/main/led_driver.c` encodes all of this:

- panel selection by `(x/32, y/8)` → chain index in `{TL=0, TR=1, BR=2, BL=3}`
- for the bottom row (panel_row=1), invert local `(lx, ly)` to `(31-lx, 7-ly)` to account for the 180° mount
- then apply the per-panel column-serpentine within the panel

The runtime switcher (`POST /api/display/map`, web UI "Pixel mapping" dropdown) keeps every legacy single-panel mode available too — handy for re-running the diagnostic patterns if a future hardware revision changes the wiring.

## Power budget on USB-C

Standard USB-C without PD negotiation provides 5 V at 3 A (15 W). With a PD trigger IC (CH224K, IP2721) it can negotiate up to 5 A.

1024 LEDs at dim amber (`R=10, G=4, B=0` per lit pixel) draws roughly 3-5 W system-wide, plus ~0.5 W for the ESP32-S3 — total well under 15 W. So a plain USB-C breakout board with **CC1 and CC2 each pulled to GND through 5.1 kΩ** is sufficient. Those 5.1 kΩ resistors tell the source "I'm a 5 V sink, give me default USB power." No PD chip needed for this use case. For brighter colours or full white, the 15 W ceiling becomes the constraint and a PD trigger configured for 5 V/3 A makes sense.

**Measured (2026-05-20, single panel, prototype rig).** 256 LEDs all lit at stored amber (R=80, G=32, B=0) with `brightness=255` (i.e. full unscaled output) drew **7.7 W** at the USB-C input. **That figure is for one of the four panels**, taken before the full grid was assembled. Linear extrapolation to the 4-panel build (now built) gives **~31 W at the same intensity** — well over the 15 W default-USB ceiling, so a fully-lit white/amber rectangle is not survivable on a non-PD source. The intended operating point is dim though: at `brightness=32` the displayed colour is ~(10, 4, 0), about an eighth of the measured amber → roughly 1 W per panel → **~4 W total for the full grid**, which sits comfortably inside default USB-C.

Action items on the power budget:

- Re-take the measurement on the assembled 4-panel grid in two regimes — `brightness=32` (operating point) and `brightness=255` (worst case) — and overwrite the projections above with the actual figures.
- Keep the firmware brightness cap in place as defence-in-depth (default `LED_DEFAULT_BRIGHTNESS=32`); a brightness slider that the user can push to 255 across all four panels will trip a non-PD source.
- If a future mode needs full-bright amber across all four panels, plan for a 5 V/5 A PD trigger (CH224K / IP2721) or a hard ceiling in `led_driver_set_brightness`.

Defence in depth: cap global brightness in firmware so a bug doesn't fry the supply. Implemented as `led_driver_set_brightness(uint8_t)` in `Firmware/main/led_driver.c`, default 32/255, applied as a scaling factor before every refresh. The web UI's brightness slider drives this through `POST /api/display/brightness`. (No FastLED in this build — pure ESP-IDF + the `espressif/led_strip` managed component.)

## Panel connectors and PCB-side connectors

The flexible WS2812B panels use **3-pin JST-SM (2.5 mm pitch)** for signal in/out and a separate **2-pin** dedicated power feed. The 3-pin signal connector carries red (5 V), green (data), white (GND). DIN on input, DOUT on output; 5 V and GND pass through for convenience but should not be relied on for powering downstream panels — voltage drops accumulate across thin signal wires.

On the PCB side use **JST-XH (also 2.5 mm pitch)**: through-hole, mechanically secure, easy to hand-solder, universally stocked. JST-XH and JST-SM housings are not cross-compatible despite the matching pitch, so short pigtail adapters (JST-SM female ↔ JST-XH female) bridge them. Alternative: board-mount JST-SM headers exist but are less common in component catalogs.

Per panel on the PCB: one 3-pin JST-XH for signal, one 2-pin JST-XH for dedicated power. With 4 panels plus one 2-pin for USB-C VBUS input, that's 4×3-pin + 5×2-pin JST-XH headers total.

The Fritzing source and exported Gerbers for the current board revision (**v3**) live in `PCB/` alongside the firmware tree — see `PCB/RGB display_v3.fzz` for the editable source and `PCB/Gerber files/` for the fabrication output.

## Firmware state (2026-05-20)

The repo contains both the firmware (`Firmware/`) and the PCB design files (`PCB/`) — what was a firmware-only repo has been promoted to a whole-project repo. The firmware builds with ESP-IDF v6.0.1 and targets `esp32s3`:

```sh
cd Firmware
idf.py set-target esp32s3 && idf.py build
```

Flashed and verified on the prototype board, **now with the full 64×16 / 1024-LED grid**.

What's running on the device:

- **LED driver**: `espressif/led_strip` ^3.0.0 over RMT, GRB, GPIO 16. Frame buffer in RAM; brightness scaled before every `led_strip_refresh`. Pixel mapping switchable at runtime, default `LED_MAP_GRID_2X2_COL_SERP` — handles the four-panel chain TL→TR→BR→BL with the bottom panels mounted upside-down. `PANEL_WIDTH`/`PANEL_HEIGHT` (32×8) and `DISPLAY_WIDTH`/`DISPLAY_HEIGHT` (64×16) in `Firmware/main/led_driver.h` make it cheap to retarget to a single panel or a different grid shape.
- **Text**: built-in 5×7 ASCII font (32–126) in `Firmware/main/display.c`, integer scale knob (default 2 → 10×14 glyphs on the 16-row grid), vertically centred. Static (centred when it fits) or right-to-left scrolling via a FreeRTOS task. Default amber stored as (80, 32, 0) so brightness=32 → displayed ~(10, 4, 0) per the original brief.
- **Persistence**: `Firmware/main/settings.c` mirrors every user-settable value (text, colour, brightness, scroll, speed, font scale, pixel mapping, boot-screen choice) into NVS namespace `cfg`. Boot order is: load settings → apply mapping + brightness → init display → **1 s amber border splash** → WiFi up → restore stored text (or fall back to the IP banner if nothing is stored, or honour `boot_screen = ip|none`) → HTTP up.
- **WiFi**: STA with NVS-stored credentials, 10 s connect timeout, fallback to open AP `LEDDisplay-Setup` + custom UDP DNS hijack on port 53 for captive-portal behaviour.
- **Web UI**: single embedded HTML/CSS/JS page served from flash on `GET /`. Mobile-friendly, no frameworks. Tabs: Display / Schedule / WiFi / System / Calibration. The Calibration tab houses the test patterns (`Step strip`, `Step (x,y)`, `Corners`, `Border`, `Fill`) and the mapping dropdown — useful when the hardware arrangement changes.
- **REST API**: `/api/status`, `/api/display/{text,brightness,clear,test,map}`, `/api/wifi/{scan,connect,reset}`, `/api/schedule`, `/api/system/{reboot,boot_screen}`. Partial bodies to `/api/display/text` preserve whatever wasn't sent — no field gets reset by omission.

Next up: re-take the power measurement on the assembled 4-panel grid (see the Power budget section), then revisit the deferred items — OTA, clock/countdown modes, MQTT integration, and an alternate 8×16-style font loaded from a public-domain bitmap.
