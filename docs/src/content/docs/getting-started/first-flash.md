---
title: First flash
description: Build the firmware with ESP-IDF and flash it over USB
sidebar:
  order: 4
---

The first flash goes over USB. Every update after that can go [over the air](../../admin-guide/ota-updates/).

## Prerequisites

- ESP-IDF v6.0.1 with the esp32s3 toolchain installed ([Espressif's setup guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html))
- The device connected over its native USB port

## Build and flash

```sh
cd Firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Replace `/dev/ttyACM0` with your serial port (`COMx` on Windows). The first build pulls the managed components (`espressif/led_strip`, `espressif/cjson`) automatically.

On a Windows host with the toolchain inside WSL, `Firmware/tools/wsl_build.sh` mirrors the source tree into the WSL filesystem and builds there, keeping the build directory out of the checkout:

```sh
wsl -d Debian -- bash -lc 'bash /mnt/z/git/led-display/Firmware/tools/wsl_build.sh'
```

## Upgrading from firmware v1

:::caution[One-time USB flash required]
v2 changed the partition table from a single `factory` app to two OTA slots (`ota_0`, `ota_1`), so a v1 device cannot take v2 over the wire; this one upgrade must go over USB with `idf.py flash`. The NVS partition kept its v1 offset and size, so WiFi credentials and display settings survive. The stored v1 pixel-mapping mode migrates to a v2 layout automatically on first boot.
:::

## Verifying the build

Host-side unit tests cover the pure modules (pixel mapping, value formatting, the fonts, the saved-network ordering, the device-name rules, the light-sensor curve) without any hardware:

```sh
cd Firmware/test/host
sh run_tests.sh
```

They compile with plain gcc and must end with `ALL HOST TESTS PASSED`.
