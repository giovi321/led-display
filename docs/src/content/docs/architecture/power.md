---
title: Power budget
description: Measured power draw, USB-C limits, and the calculated current budget that keeps the display inside them
sidebar:
  order: 3
---

The reference build is 1024 WS2812B LEDs (a 2x2 grid of 32x8 panels) powered from a single USB-C input at 5 V. The intended use is a dim, always-on readout of one value or a short message, and the power design leans on that: dim operation fits default USB-C with margin, full brightness does not.

## USB-C limits

Standard USB-C without PD negotiation provides 5 V at 3 A (15 W). The board requests that by pulling CC1 and CC2 to GND through 5.1 kOhm resistors each; no PD chip is involved. With a PD trigger IC (CH224K, IP2721) the input could negotiate up to 5 A at 5 V (25 W), which is the upgrade path if a future mode needs bright output across all panels.

## Measured draw

Measured on 2026-05-20 on the prototype rig, single panel, before the full grid was assembled: 256 LEDs all lit at the stored amber (R=80, G=32, B=0) with brightness=255 (full unscaled output) drew 7.7 W at the USB-C input.

Everything below is extrapolation from that one measurement, not measured on the assembled grid:

- linear extrapolation to 4 panels at the same intensity gives roughly 31 W, well over the 15 W default USB-C ceiling, so a fully lit rectangle at brightness 255 is not survivable on a non-PD source
- at the intended operating point (brightness=32, displayed color around R=10, G=4, B=0) the same scaling gives roughly 1 W per panel, about 4 W for the full grid, comfortably inside default USB-C

The ESP32-S3 itself adds an estimated 0.5 W (estimate from the project notes, not separately measured).

The 4 W figure is optimistic, because scaling one lit measurement linearly folds the quiescent draw into the scaling. The per-LED controller current does not scale with brightness at all (see below), so at 1024 LEDs it is a fixed floor of about 1 A on its own.

Open item: re-measure on the assembled 4-panel grid in both regimes, brightness=32 (operating point) and brightness=255 (worst case), and replace the projections above with actual figures.

## The current model

A WS2812B's draw is linear in the byte values clocked out to it: a fixed quiescent current for the controller inside each LED, plus a share of the full-scale channel current for each of R, G and B. That makes a whole frame's draw a function of one number, the sum of every channel byte in it:

```
I = px * I_quiescent + (sum_of_channel_bytes / 255) * I_channel_full
```

`power_model.[hc]` is that formula and nothing else: integer-only, free of ESP-IDF, and host-tested under `Firmware/test/host/`. The web UI carries the same arithmetic in `app.js` so moving the brightness slider is priced without a round trip; the two are kept in step deliberately, and both are exercised against the same expected values.

Three constants, all configurable through `POST /api/display/power` and the Hardware tab, because panels sold as "WS2812B" vary and the only honest way to pin them down is to measure the chain you own:

| Constant | Default | Where the default comes from |
|---|---|---|
| Current per colour channel at 255 | 20 mA | WS2812B datasheet nominal |
| Quiescent current per LED | 1 mA | Datasheet idle draw of the controller IC |
| Budget for the LEDs | 2600 mA | 3 A default USB-C less roughly 400 mA for the ESP32-S3 and margin |

The channel default is deliberately not the value fitted to the measurement above. Solving the model for that 7.7 W point gives about 10.5 mA per channel, half the datasheet figure. Defaulting to the fitted value would make the estimate accurate on the panels that were measured and wrong in the dangerous direction on any panel closer to the datasheet: the limiter would let the supply trip while reporting headroom. Over-predicting costs some brightness. Under-predicting costs the feature its entire purpose.

Two consequences of the defaults worth knowing before the numbers surprise you:

- 1024 LEDs draw about 1 A with every channel off. That is 40% of the default budget spent before anything is lit, and it is real: a long chain of WS2812Bs is never free.
- because the light term is doubled relative to the fitted constant, the estimate at the normal operating point reads roughly twice what the extrapolation earlier on this page suggests. Calibrate if that matters to you.

### Calibrating

The estimate is only as good as the constants, and both are measurable with a meter in the supply lead. The Fill pattern under Diagnostics is the useful one here because it lights every LED at a fixed, known colour - (80, 32, 0), a channel-byte sum of 112 per LED - regardless of what the colour picker says.

Lower the brightness before running Fill. Every LED lit is the worst case the chain has, which is exactly the condition this page exists to warn about.

Three readings:

1. **Controller only.** Disconnect the panels' power and read the input. That is the ESP32-S3's own draw, which the LED budget deliberately excludes.
2. **Panels connected, display cleared.** Subtract reading 1. What is left is the quiescent draw of the whole chain, so divide by the number of LEDs for the per-LED constant.
3. **Fill at a brightness `B`.** Subtract reading 2. What is left is purely the light term, so

   ```
   I_channel_full = I_light * 255 * 255 / (px * 112 * B)
   ```

   with `I_light` and the result in the same unit. At 1024 LEDs and `B` = 32, a measured light term of 400 mA gives 400 * 65025 / (1024 * 112 * 32) = about 7.1 mA per channel.

A quicker sanity check without the arithmetic: run Fill and compare the meter against the estimated draw on the Appearance tab, then scale the per-channel constant by the ratio between them. That folds any error in the quiescent constant into the channel constant, which is fine while the display is mostly lit and wrong when it is mostly dark - so prefer the three readings if you are going to rely on the limiter.

## Enforcement

Two mechanisms sit on top of the brightness, and they are not the same thing.

**The brightness cap** (`POST /api/display/brightness_cap`, 0..255, 255 = uncapped) is a blunt ceiling. The effective brightness is the user setting clamped by the cap, so a limit configured once survives careless API calls, a user dragging the slider to 255, or a bug that writes a large brightness value. It scales RGB without knowing what the frame will draw, which makes it defence in depth rather than a current limit.

**The power limiter** is the calculated version, and it is off by default. With it on, every refresh sums the frame's channel bytes, prices them, and if the result exceeds the budget solves the model backwards for the highest brightness that fits, then sends the frame at that brightness instead. The clamp is per frame and presentation only: the stored brightness is never rewritten, so a shorter string or a dimmer colour gives the level back without anyone touching the slider. `GET /api/status` reports `power.limited` and the brightness it settled on while this is happening.

The offline marker pixel is priced into the estimate but never scaled down. It has its own brightness precisely so it stays readable on a dimmed panel, and a power limiter that dimmed the "this display is offline" indicator would be defeating its own purpose.

When the quiescent draw of the chain alone exceeds the budget, `power.floor_exceeded` says so and the answer is 0: no brightness helps, because the LEDs draw that current with every channel off. Raise the budget if the supply can take it, or drive fewer panels.

Both the cap and the budget persist in NVS and are restored at boot in the safe order - budget and cap before the brightness - so a value over either one never reaches the panels even for one frame. Devices stored before the budget existed load the defaults with the limiter off, so nothing about an existing display changes on update.

## Firmware updates blank the panel

A firmware update is the one moment the current model cannot protect you, because the overdraw does not come from the content at all.

The strip is driven from an RMT channel without DMA, and its interrupt is not IRAM-safe. Writing flash disables the cache on both cores, so that interrupt cannot refill its 64-symbol buffer while an erase or a write is in progress. A stall longer than the WS2812B's roughly 50 us reset window makes the strip latch and begin a new frame mid-stream: the remaining bits land at the wrong LED and, worse, at the wrong bit offset inside each byte. Misalignment at sub-byte granularity assembles large channel values out of small ones, so a dim amber frame comes out as a scatter of bright LEDs. That is the garbage on the panel, and the current spike that trips the supply.

`OTA_WITH_SEQUENTIAL_WRITES` erases as the image arrives rather than once at the start, so there is no quiet window later in the transfer. A display set to scroll continuously at high speed transmits almost without gaps and collides with every erase.

Both update paths therefore freeze the strip before the first flash operation: one static frame goes out at a fixed low brightness, and then nothing at all. A frame nobody retransmits cannot be torn, and a frame that is already dark cannot be corrupted upward either, since misaligned zero bits are still zero. That second property is why freezing is the fix rather than a mitigation, and why it is stronger than lowering the brightness - brightness scaling leaves nonzero bytes to be misread.

The frozen frame is left latched through the reboot on purpose. The LEDs keep their own power across a restart, so the notice stays on the panel until the new image clears the strip, instead of bright garbage surviving into the next boot. A failed update thaws and repaints.

One consequence worth knowing: updating **from** a build older than 2.11.0 still hits this, because the old image is the one driving the strip while the new one is being written. Clear the display first (Display tab, Clear) and the frame going out is already black, which is enough to get through the one update that installs the fix.

## Warning before the fact

The failure this exists to prevent is not gradual. A colour and brightness that draw too much cut the supply out, the display goes dark, and it cuts out again as soon as the text is redrawn - so recovering means unplugging panels, power-cycling, lowering the brightness and reconnecting them.

The Appearance tab therefore prices the slider before you apply it. It shows the estimated draw of the content actually on the panel at the slider's position, a bar against the budget, and, underneath, what every pixel lit at that colour would cost. Crossing the budget turns the figure red, opens a warning that names both numbers and the consequence, and makes Apply ask a second time.

The estimate is deliberately based on the content on screen rather than the all-pixels-lit worst case. At the normal operating point a worst-case warning would be on almost permanently, and a warning that is always on is a warning nobody reads.

Where the cap and the budget sit in the code and the boot sequence is covered in [firmware architecture](../firmware/).
