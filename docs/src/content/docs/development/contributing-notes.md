---
title: Contributing notes
description: Repo layout, code style, how to add an API endpoint, and what a PR should look like
sidebar:
  order: 2
---

Short and factual: what you need to know before touching the code.

## Repo layout

| Path | Content |
| --- | --- |
| `Firmware/main/` | all application sources, one module per concern |
| `Firmware/main/web/` | web UI sources, embedded at build time |
| `Firmware/components/esp_wireguard/` | vendored WireGuard component (`PORTING.md` documents provenance and local changes) |
| `Firmware/test/host/` | host unit tests for the pure C modules |
| `Firmware/tools/` | `gzip_asset.py` (asset pipeline), `wsl_build.sh` (WSL build helper) |
| `PCB/` | Fritzing source and exported Gerbers |
| `3D/` | enclosure parts: DXF outlines for cutting, STL for printing |
| `docs/` | this documentation site (Astro Starlight) |
| `DESIGN.md` | the v2 design document; JSON shapes and API contracts reference it |

## Code style

Match the existing modules; there is no separate style config:

- C11, 4-space indent, file-scope statics prefixed with `s_`, one module prefix per file (`led_driver_*`, `mapping_*`, ...)
- comments explain why, not what; module headers state ownership and threading expectations
- pure logic goes in ESP-IDF-free modules (`mapping.c`, `valuefmt.c` are the pattern) so it stays host-testable; JSON and NVS handling stay in the API and settings layers
- LF line endings everywhere

## Adding an API endpoint

Each `api_*.c` file owns its URI space exclusively; the ownership table is in `api_handlers.h`. To add an endpoint:

1. Put the handler in the `api_*.c` module that owns the path, and register the URI in that module's `api_*_register` function
2. For a new domain, create a new `api_*.c`: add it to `SRCS` in `Firmware/main/CMakeLists.txt`, declare its register function in `api_handlers.h`, and call it from `web_server.c` (before `web_static_register`, which must stay last)
3. Use the `web_common.h` helpers for body reading, JSON responses, and errors; mutating endpoints call `web_require_auth` first
4. If the endpoint persists config, store it as a JSON blob via `settings_save_blob` (document the key in `settings.h`) and add it to `/api/config/export` and `/api/config/import` in `api_config.c`
5. Wire the UI in `Firmware/main/web/app.js` (and `index.html` if it needs markup); all requests go through `window.apiFetch`, which handles the API key
6. Update the API reference page in `docs/`

## PR expectations

- host tests pass (`Firmware/test/host/run_tests.sh`); changes to `mapping.c` or `valuefmt.c` come with test updates
- the firmware builds clean with ESP-IDF v6.0.1 for `esp32s3` (see [building](../building/))
- no build artifacts, `sdkconfig`, or `managed_components/` in the diff
- small, focused commits; docs updated in the same PR as the code they describe
- endpoint or blob changes stay consistent with the shapes in `DESIGN.md`, or update it
