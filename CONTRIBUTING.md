# Contributing

Small project, simple rules. Open an issue first for anything bigger than a bugfix so the approach is agreed before you write code.

## Firmware setup

The firmware builds with ESP-IDF v6.0.1 for the esp32s3 target:

```sh
cd Firmware
idf.py set-target esp32s3
idf.py build
```

On a Windows host with the toolchain in WSL, use the helper instead; it mirrors the tree into the WSL filesystem (network-mount builds are slow and fragile) and builds there:

```sh
bash Firmware/tools/wsl_build.sh          # build
TESTS=1 bash Firmware/tools/wsl_build.sh  # build + host tests
```

## Host tests

The pure modules (`mapping.c`, `valuefmt.c`) have host-side unit tests that need nothing but gcc:

```sh
cd Firmware/test/host
./run_tests.sh
```

They must pass before any PR. If you change `mapping.c` or `valuefmt.c`, extend the tests to cover the change.

## Module style

- Pure logic (no ESP-IDF, no FreeRTOS, no cJSON includes) goes in host-testable modules like `mapping.c` and `valuefmt.c`; JSON and NVS handling stay in the API and settings layers
- Each HTTP endpoint is owned by exactly one `api_*.c` file; check `Firmware/main/api_handlers.h` for the ownership table before adding a handler
- Comments explain why, not what
- Match the formatting of the file you are editing

## Docs

The documentation site is Astro Starlight under `docs/`:

```sh
cd docs
npm install
npm run dev
```

`npm run build` must pass; the GitHub Pages workflow runs it on every push to `main` that touches `docs/`.

## Pull requests

- One feature or fix per PR
- Update the docs and CHANGELOG.md when behaviour changes
- No real names or real email addresses in commits; use your handle and a GitHub noreply address
