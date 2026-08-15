---
title: Releasing
description: Cut a firmware release so the build lands on GitHub as led_display.bin and devices find it through their update check
sidebar:
  order: 3
---

A release exists so devices can update themselves. The device's [update check](../../admin-guide/ota-updates/) reads the latest release of the configured repository and looks for one asset named exactly `led_display.bin`. Producing that asset is the whole job of the release workflow (`.github/workflows/release.yml`); everything below is about giving it a tag it accepts.

## The asset name is a contract

`Firmware/main/ota_update.c` defines `GH_ASSET_NAME "led_display.bin"` and matches release assets against it with `strcmp`, so the comparison is exact and case-sensitive. A release whose asset is called `firmware.bin`, `led_display-2.1.0.bin`, or anything else makes `GET /api/ota/check` fail with "release has no led_display.bin asset", and the System tab offers no update even though a newer version exists.

The name is not chosen by the workflow. It comes from `project(led_display)` in `Firmware/CMakeLists.txt`, which makes the build write `Firmware/build/led_display.bin`. Renaming the CMake project, or renaming the file on the way to the release, breaks the update check for every device already in the field. If it ever has to change, `GH_ASSET_NAME` has to change with it, and devices running older firmware will keep looking for the old name.

## Cutting a release

1. Bump `FW_VERSION` in `Firmware/main/fw_version.h` to the new version, without the leading `v`

   ```c
   #define FW_VERSION "2.11.0"
   ```

2. Add the matching section to `CHANGELOG.md`, following the Keep a Changelog headings already in the file
3. Commit both on `main`
4. Tag that commit and push the tag:

   ```sh
   git tag v2.11.0
   git push origin v2.11.0
   ```

5. Watch the Release workflow in the Actions tab. It checks out the tag, verifies the tag matches `FW_VERSION`, builds with ESP-IDF v6.0.1 for `esp32s3`, and creates (or updates) the release for that tag with generated release notes and `led_display.bin` attached
6. Confirm the release page lists `led_display.bin`, then check a device: System tab, "Check GitHub for updates". It should offer the new version and install it in one click

The version check is the first step for a reason. If the tag and `fw_version.h` disagree the workflow fails immediately with the mismatch spelled out, before spending several minutes on a build that would ship a binary reporting the wrong version to `/api/status`, the UI footer and every other device's update check.

## Publishing from the web UI

Publishing a release from the GitHub web UI works too: it creates the tag, which fires the same workflow, and the workflow then attaches the binary to the release that already exists.

Two habits keep that path reliable:

- publish the release rather than saving it as a draft. A draft does not create the tag, so nothing runs until it is published
- let the workflow write the release notes, or write your own and leave the asset to the workflow. Do not upload a hand-built binary under a different name

## When a release has no binary

The workflow only runs on a tag push. A release can therefore end up without its asset:

- the tag was already on the remote before the release was created, so no push event happened
- more than three tags were pushed in one go, and GitHub emitted no push events for them
- the workflow file was broken, or the build failed, at the time the tag went up

The fix in all three cases is the manual path: Actions tab, Release workflow, "Run workflow", enter the tag name. It checks out that tag, runs the same version check and build, and attaches the binary to the existing release. Nothing needs to be re-tagged.

## Prereleases

A tag with a suffix, `v2.11.0-rc1`, is published as a prerelease. GitHub's `releases/latest` skips prereleases, and that is the endpoint the device queries, so a release candidate stays invisible to devices until a plain `vX.Y.Z` tag follows. Use it to test the full release path without offering the build to anything in the field.

## What the workflow does not do

There is no build cache. `espressif/esp-idf-ci-action` runs the build inside the ESP-IDF Docker image and exposes no cache input, so every release pulls the image and builds from scratch, which takes a few minutes. Caching the build directory across runs is not worth the failure modes it introduces; a release happens rarely enough that the wait costs nothing.
