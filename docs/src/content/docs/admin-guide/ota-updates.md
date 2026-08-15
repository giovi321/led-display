---
title: OTA updates
description: Update the firmware from the browser or from a URL, with two flash slots and automatic rollback when a new image fails to boot
sidebar:
  order: 5
---

From firmware v2 onward the device updates itself over WiFi. The flash holds two 6 MB application slots, `ota_0` and `ota_1`; an update streams into whichever slot is not running, and the bootloader switches over only after the image has been fully written and validated. The display keeps working while the update streams in.

## One-time prerequisite

A device still running v1 firmware has a single `factory` app partition and cannot receive the v2 partition table over the air. It needs one final flash over USB to install the OTA layout; see [first flash](../../getting-started/first-flash/). Stored settings and WiFi credentials survive, because the settings partition keeps its v1 offset. After that one USB flash, every subsequent update goes through the browser or the URL endpoint.

## Path 1: browser upload

System tab ([advanced mode](../advanced-mode/)), Firmware update card. Pick the `led_display.bin` produced by the build and press Upload. The UI shows upload progress, waits through the reboot, and reports the new version once the device answers again.

The endpoint takes the raw image as the POST body, no multipart wrapping, so it also works from the command line:

```bash
curl -X POST --data-binary @led_display.bin \
  -H 'X-Api-Key: KEY' http://<device-ip>/api/ota/upload
```

## Path 2: update from URL

The device can pull the image itself, which is the path meant for n8n flows, CI pipelines, or any other automated rollout: host the `.bin` somewhere reachable and POST the link.

```bash
curl -X POST http://<device-ip>/api/ota/url \
  -H 'X-Api-Key: KEY' -d '{"url":"http://build-server/led_display.bin"}'
```

The request returns immediately; the download and flash run in the background. `https://` URLs are verified against the ESP-IDF certificate bundle. Plain `http://` URLs are accepted too, which is the practical choice for a build server on your own LAN.

## Path 3: GitHub releases

The System tab has a "Check GitHub for updates" button. The device queries the latest release of its configured repository (`CONFIG_LEDDISP_GITHUB_REPO`, default `giovi321/led-display`), compares the release tag against the running firmware version, and offers a one-click install of the release's `led_display.bin` asset. Under the hood this is the URL path: the install button POSTs the asset URL to `/api/ota/url`.

The same check is scriptable: `GET /api/ota/check` returns the current and latest versions plus the asset URL, so an automation can poll it and trigger `/api/ota/url` itself.

Releases are produced by the repository's release workflow: pushing a tag `vX.Y.Z` builds the firmware with ESP-IDF in CI (the tag must match `FW_VERSION` in `Firmware/main/fw_version.h` or the build fails) and attaches `led_display.bin` to the GitHub release. The repository must be public for the device to reach the release; GitHub asset downloads redirect to a second host, which the updater follows.

## Progress and status

`GET /api/ota/status` is open (no key) and reports:

```json
{
  "running_version": "2.10.0",
  "running_slot": "ota_0",
  "last_result": "ok: 2.10.0 flashed to ota_1, rebooting",
  "in_progress": false,
  "progress_pct": 100
}
```

`progress_pct` is -1 while the size is unknown. `last_result` keeps a human-readable outcome of the most recent attempt, including failures ("image validation failed: not a bootable app image for this chip"), so it is the first thing to check when an update did not take. The web UI polls this endpoint during updates. If the `ota_result` webhook event is enabled under Integrations, the device also POSTs the outcome there.

Only one update can run at a time; a second attempt while one is in progress gets a 409. An image larger than the 6 MB slot is rejected with 413 before anything is written.

## Rollback

Bootloader rollback is enabled. A freshly flashed image boots in a pending-verify state and must confirm it works; the firmware marks itself valid only after its web server is up. If the new image crashes or hangs before reaching that point, the next reset falls back to the previous slot and the device comes back on the firmware it ran before. A failed update therefore costs a reboot, not a bricked device.

The update endpoints are POSTs, so they require the [API key](../api-key/) when one is set. `GET /api/ota/status` stays open either way.
