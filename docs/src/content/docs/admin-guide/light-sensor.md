---
title: Light sensor
description: Wire an analog light sensor to an ADC pin and let the room set the display brightness, calibrated against the live reading
sidebar:
  order: 3
---

The firmware can read the brightness off the room instead of taking it from the slider: one analog light sensor on an ADC pin, two calibration points, and the display follows the light in the room, bright enough to read in daylight and dim at night. The sensor is optional and off by default. It is configured in the Hardware tab ([advanced mode](../advanced-mode/)), or at [`GET/POST /api/display/light_sensor`](../../api-reference/display/#get-apidisplaylight_sensor).

## Wiring

Any sensor that produces a voltage works: a photoresistor (LDR) in a divider, or an analog lux module with a voltage output. The reference recipe is the divider:

| Connection | Detail |
|------------|--------|
| 3.3 V | one leg of the LDR (use the board's 3.3 V, not the 5 V LED rail) |
| Divider node | the LDR's other leg, the fixed resistor, and the ADC pin all meet here |
| GND | the free end of the fixed resistor |

Pick the fixed resistor in the 10 kOhm to 100 kOhm range, roughly matching what the LDR measures at the light level you care about; that puts the interesting part of the swing in the middle of the ADC range. Which leg goes to 3.3 V and which to ground decides whether the reading rises or falls with light, and both work: the dark reading may be the higher of the two numbers, and a reversed pair maps the right way round on its own. There is no separate inversion setting to get wrong.

Only GPIO1 to GPIO10 are offered. Those are the ESP32-S3's ADC1 pins; ADC2 shares hardware with the WiFi radio and cannot be read reliably on a device that is online. Keep the sensor wire away from the LED data run, and mount the sensor where it sees the room rather than the panel: an LDR staring at its own display feeds itself.

## Calibration

The whole point of the live reading in the UI is that you never have to compute anything. The device samples the pin once a second as soon as a valid pin is saved, even with the sensor still switched off.

1. Pick the pin under Sensor pin and press Save light sensor. The live reading appears and starts updating
2. Cover the sensor, wait for the reading to settle, and press Use current next to Dark reading
3. Light the sensor as brightly as the room ever gets, wait again, and press Use current next to Bright reading
4. Set Brightness when dark and Brightness when bright: the two ends of what the panel should do. The upper end is still subject to the [brightness cap](../../architecture/power/)
5. Adjust the response curve if the middle feels wrong. 1.0 is a straight line between the two ends, below 1.0 brightens as soon as any light appears, above 1.0 keeps the panel dim until the room is properly lit
6. Tick "Set brightness from the sensor" and save

The two calibration readings must differ, and enabling the sensor needs a pin: the device rejects a configuration that fails either rule instead of storing something that cannot produce a brightness.

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
