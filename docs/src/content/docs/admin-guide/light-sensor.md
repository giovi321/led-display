---
title: Light sensor
description: Wire a BH1750 lux meter or an analog LDR divider and let the room set the display brightness, calibrated against the live reading
sidebar:
  order: 3
---

The firmware can read the brightness off the room instead of taking it from the slider: a light sensor, two calibration points, and the display follows the room, bright enough to read in daylight and dim at night. The sensor is optional and off by default. It is configured in the Hardware tab ([advanced mode](../advanced-mode/)), or at [`GET/POST /api/display/light_sensor`](../../api-reference/display/#get-apidisplaylight_sensor).

Two kinds of sensor are supported and neither is preferred:

- **BH1750**, a digital lux meter on I2C, usually sold as the GY-302 module. The v4 controller board has a connector for it. It reports real lux, so its numbers mean something outside this device
- **Analog**, a photoresistor divider or any module with a voltage output, on an ADC pin. This is what the firmware supported first and it remains a first-class option: no extra module to buy, and it works on a v3 board

Everything after the reading is shared. The same two calibration points, the same response curve, the same smoothing, the same handover to the brightness arbiter. Switching from one to the other means re-capturing the two points, because the numbers are on different scales, and nothing else.

## Wiring the BH1750

The v4 board carries a 4-way connector silkscreened `GND SDA SCL 5V`. The module plugs into it.

| Board pin | GY-302 pin | Note |
|---|---|---|
| `GND` | GND | |
| `SDA` | SDA | Reaches GPIO8 |
| `SCL` | SCL | Reaches GPIO9 |
| `5V` | VCC | The module has its own 3.3 V regulator, so it takes the board's 5 V rail directly |

Two things the connector does not do for you:

- **The GY-302's pin order is GND, ADDR, SDA, SCL, VCC** - five pins against the connector's four, and ADDR sits second. Wire pin to pin by name, not by position
- **ADDR must be tied to GND** for the sensor to answer at address `0x23`, which is the only address the firmware talks to. That link is made on the module, not on the board: bridge its ADDR pin to its GND pin

The pins are configurable if you wire it somewhere else, but GPIO8 and GPIO9 are what the board's connector reaches, so leave them alone unless you have a reason. The UI will not offer pins the module needs for its own flash and PSRAM, or the strapping pins that are read at reset.

Mount it facing the room, not the panel. A sensor looking at its own display feeds itself: the panel brightens, the sensor reads more light, the panel brightens again.

## Wiring an analog sensor

Any sensor that produces a voltage works: a photoresistor (LDR) in a divider, or an analog lux module with a voltage output. There is no footprint for this on the board, so it wires to the ESP32 header directly. The reference recipe is the divider:

| Connection | Detail |
|------------|--------|
| 3.3 V | one leg of the LDR (use the board's 3.3 V, not the 5 V LED rail) |
| Divider node | the LDR's other leg, the fixed resistor, and the ADC pin all meet here |
| GND | the free end of the fixed resistor |

Pick the fixed resistor in the 10 kOhm to 100 kOhm range, roughly matching what the LDR measures at the light level you care about; that puts the interesting part of the swing in the middle of the ADC range. Which leg goes to 3.3 V and which to ground decides whether the reading rises or falls with light, and both work: the dark reading may be the higher of the two numbers, and a reversed pair maps the right way round on its own. There is no separate inversion setting to get wrong.

Only GPIO1 to GPIO10 are offered. Those are the ESP32-S3's ADC1 pins; ADC2 shares hardware with the WiFi radio and cannot be read reliably on a device that is online. Keep the sensor wire away from the LED data run, and mount the sensor where it sees the room rather than the panel: an LDR staring at its own display feeds itself.

## Calibration

The whole point of the live reading in the UI is that you never have to compute anything. The device samples once a second as soon as a usable sensor is saved, even with the sensor still switched off, and the steps are the same for both kinds.

1. Choose the sensor under Sensor, check the pins below it, and press Save light sensor. The live reading appears and starts updating. A BH1750 reports lux; an analog sensor reports raw ADC counts out of 4095
2. Cover the sensor, wait for the reading to settle, and press Use current next to Dark reading
3. Light the sensor as brightly as the room ever gets, wait again, and press Use current next to Bright reading
4. Set Brightness when dark and Brightness when bright: the two ends of what the panel should do. The upper end is still subject to the [brightness cap](../../architecture/power/)
5. Adjust the response curve if the middle feels wrong. 1.0 is a straight line between the two ends, below 1.0 brightens as soon as any light appears, above 1.0 keeps the panel dim until the room is properly lit
6. Tick "Set brightness from the sensor" and save

The two calibration readings must differ, enabling an analog sensor needs a pin, and SDA and SCL must be different pins: the device rejects a configuration that fails any of those instead of storing something that cannot produce a brightness.

If the live reading says "no BH1750 answering on those pins", the module is not replying at `0x23`. In order of likelihood: ADDR is not bridged to GND on the module, SDA and SCL are swapped, or the connector is not seated. The device keeps polling, so fixing the wiring brings it back without a reboot.

## How it behaves once enabled

The reading is smoothed, and a new level only reaches the panel when it has moved a few steps, so a passing shadow or a cloud does not make the display flicker.

The sensor level replaces your stored brightness while the [schedule](../../user-guide/schedule/) allows the display to be on. Everything else keeps its say:

- outside the on window the schedule wins, blank or dimmed as configured
- the brightness cap clamps the result, always
- a [stale value](../../user-guide/value-updates/#the-staleness-guard) halves what is left after the cap, so the staleness dim stays visible even on a capped display
- in [setup-AP mode](../../getting-started/wifi-setup/) the sensor is bypassed and the stored brightness is used, so a dark room cannot dim away the banner carrying the setup SSID and address

Your manually set brightness is stored, not overwritten: `GET /api/status` reports it as `display.brightness_stored` next to the applied `display.brightness`. The Display tab's slider shows the sensor's level and stops accepting input once the sensor has produced one; it stays usable before the first reading and in setup-AP mode. Switch the sensor off and your own level comes back.

## Related pages

- [Display](../../user-guide/display/): the brightness slider and what disables it
- [Hardware](../../getting-started/hardware/): parts and wiring of the reference build
- [Backup and restore](../backup-restore/): the sensor configuration travels in the config file
