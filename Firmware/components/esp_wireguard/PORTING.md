# esp_wireguard porting notes

## Origin

Vendored from https://github.com/droscy/esp_wireguard (an actively maintained
fork of https://github.com/trombik/esp_wireguard), branch `dev`, commit
`b737275755f5d5a32da81ad25ca5e3834dec2719` (release 0.4.5). The fork already
carries the fixes for ESP-IDF 6.0+ and mbedtls 4.0+ (upstream commit
`ecf268e`), which is why it was chosen over trombik's original.

Copied: `LICENSE`, `include/`, `src/`. The upstream `.git`, ESPHome-focused
`README.md`, `library.json` and `TODO.md` were not carried over.

## Local changes

- `CMakeLists.txt`, `Kconfig`: written for this project. The droscy fork
  targets ESPHome/PlatformIO and dropped the ESP-IDF component scaffolding,
  so these are modeled on trombik's originals. `src` is a public include dir
  because the public header `esp_wireguard.h` includes `esp_wireguard_err.h`
  which lives there.
- `src/crypto/refc/x25519.{c,h}`, `x25519-license.txt`: restored from
  upstream history (`a00bca0^`). The fork replaced the reference X25519 with
  libsodium because the reference code faults on ESP8266 (unaligned loads);
  ESP32-S3 handles unaligned access, and restoring it keeps the component
  self-contained instead of pulling the libsodium managed component.
- `src/crypto.h`: X25519 section reverted to the restored reference
  implementation (undoes upstream `a00bca0` for this tree).
- `src/crypto/refc/x25519.c`: `mul()` third parameter changed from
  `const fe` to `const limb_t *`. Callers pass shorter arrays (the 1-limb
  `a24` constant) with `nb` bounding the reads; GCC 15 (ESP-IDF v6.0.1
  toolchain) flags the array-parameter bound as `-Werror=stringop-overread`.
- Line endings normalized to LF (upstream commits CRLF); no content changes
  beyond the ones listed above.

## Validation

Built green in an isolated ESP-IDF v6.0.1 project, target esp32s3, with a
test main exercising every public API entry point (so all objects link) and
with the firmware's `wg_client.c` compiled against the component. Not tested
against a live peer from this environment; the tunnel path is
runtime-verified on hardware.

## Runtime notes for callers

- The component drives raw lwIP APIs (`netif_add`, `dns_gethostbyname`,
  `udp_*`). With `CONFIG_LWIP_TCPIP_CORE_LOCKING` off (the IDF default),
  these are only thread-safe from the tcpip thread, so `wg_client.c`
  marshals every component call through `esp_netif_tcpip_exec()`.
- Single tunnel instance: the component keeps its state in file-scope
  statics (`wg_netif_struct`, `peer`, ...). One tunnel, one peer.
- Handshake timestamps (tai64n) need sane wall time or the peer rejects the
  initiation as a replay; connect only after SNTP sync (wg_client waits).
- The endpoint hostname is resolved once per `esp_wireguard_init()`. To pick
  up a changed DNS record, tear down and re-init (wg_client does this after
  a 180 s handshake stall).
- mbedtls >= 4 (IDF 6) uses the PSA path in `wireguard-platform.c`;
  `psa_crypto_init()` is idempotent, so repeated init is safe.
- Allowed IPs are IPv4 CIDRs; `esp_wireguard_add_allowed_ip()` takes dotted
  IP + dotted mask. The device's own tunnel address is auto-allowed as /32.

## License

BSD 3-Clause (see `LICENSE`); the restored X25519 keeps its own license in
`src/crypto/refc/x25519-license.txt` (MIT, Cryptography Research, Inc.).
