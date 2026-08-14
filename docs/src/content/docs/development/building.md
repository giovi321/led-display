---
title: Building
description: ESP-IDF setup, device builds, the WSL build helper, host tests, and the web asset editing loop
sidebar:
  order: 1
---

The firmware builds with ESP-IDF v6.0.1 for the `esp32s3` target. There are two build products: the device image (needs ESP-IDF) and the host unit tests (need only gcc).

## ESP-IDF setup

Install ESP-IDF v6.0.1 the standard way:

```sh
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git ~/esp-idf
cd ~/esp-idf && ./install.sh esp32s3
```

Then in every build shell:

```sh
source ~/esp-idf/export.sh
```

Dependencies resolve automatically: `espressif/led_strip` and `espressif/cjson` come from the component manager (`Firmware/main/idf_component.yml`), and the WireGuard component is vendored in `Firmware/components/esp_wireguard/` (see its `PORTING.md` for provenance).

## Device build

```sh
cd Firmware
idf.py set-target esp32s3
idf.py build
idf.py -p <port> flash monitor
```

`sdkconfig.defaults` carries the project configuration: 16 MB flash, octal PSRAM, the custom partition table (two OTA slots, see [firmware architecture](../../architecture/firmware/)), and OTA rollback. The first flash of v2 over v1 must go over USB because it installs the new partition table; updates after that can go through the web UI or `/api/ota`.

## WSL build helper

For development on a Windows host with the toolchain in WSL, `Firmware/tools/wsl_build.sh` avoids building directly on the mounted Windows drive (slow and fragile). Run it inside WSL:

```sh
bash /mnt/z/git/led-display/Firmware/tools/wsl_build.sh
```

What it does:

- mirrors the `Firmware/` tree from the Windows checkout into `~/led-display/src` with tar, excluding `build/`, `managed_components/`, `sdkconfig` and lock files
- keeps the build directory (`~/led-display/build`) and `sdkconfig` outside the mirrored tree, so a re-sync never deletes them
- sources `~/esp-idf/export.sh` and runs `idf.py -B ~/led-display/build -DSDKCONFIG=~/led-display/sdkconfig build`

Two environment variables: `SRC=` overrides the source checkout path (default `/mnt/z/git/led-display/Firmware`), and `TESTS=1` builds and runs the host tests before the device build.

## Host tests

The pure C modules (`mapping.c`, `valuefmt.c`) have unit tests that run on any machine with gcc, no ESP-IDF required:

```sh
cd Firmware/test/host
./run_tests.sh
```

The script compiles with `-std=c11 -Wall -Wextra -Werror -g` and runs both binaries; tests are plain `assert`, and the run ends with `ALL HOST TESTS PASSED`. The mapping tests include the oracle comparison against the v1 implementation described in [pixel mapping](../../architecture/pixel-mapping/). Run them after any change to the mapping or value-formatting code; they are the fast feedback loop for exactly the code that is hardest to debug on hardware.

## Web asset editing loop

The web UI sources live in `Firmware/main/web/` (`index.html`, `style.css`, `app.js`, `editor.js`) and are gzipped and embedded at build time; nothing is served from a filesystem partition. The loop is:

1. Edit the files in `Firmware/main/web/`
2. Run `idf.py build`: the assets are registered as configure dependencies, so the build re-runs the configure step, regzips the changed files (deterministically, mtime 0), and re-embeds them
3. Flash and reload the page; assets are served with `Cache-Control: no-cache`, so the browser picks up the new build without cache busting

There is no separate asset pipeline to invoke and no minification step; the files are served as written, gzip does the size work.
