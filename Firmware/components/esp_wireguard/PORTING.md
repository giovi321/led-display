# esp_wireguard porting notes

## Origin

Vendored from https://github.com/droscy/esp_wireguard (an actively maintained
fork of https://github.com/trombik/esp_wireguard), branch `dev`, commit
`b737275755f5d5a32da81ad25ca5e3834dec2719` (release 0.4.5). The fork already
carries the fixes for ESP-IDF 6.0+ and mbedtls 4.0+ (upstream commit
`ecf268e`), which is why it was chosen over trombik's original.

Copied: `LICENSE`, `include/`, `src/`. The upstream `.git`, ESPHome-focused
`README.md`, `library.json` and `TODO.md` were not carried over.

## Required project configuration

`CONFIG_ESP_NETIF_BRIDGE_EN=y` and `CONFIG_LWIP_NUM_NETIF_CLIENT_DATA=1`
(both in `sdkconfig.defaults`). Without them the tunnel panics the device the
moment it is enabled, inside `netif_add()`, before any of this component's
own code runs.

`esp_netif` stores its handle in the lwIP netif's `state` field and its global
`netif_ext_callback` casts `state` back to an `esp_netif_t *` unconditionally
(`lwip_get_esp_netif()` in `esp_netif_lwip.c`). This component adds a raw lwIP
netif whose `state` is its own `wireguardif_init_data` / `wireguard_device`,
so the first address event during `netif_add()` hands that callback a pointer
to the wrong type and it faults.

IDF's `lwipopts.h` states the rule - interfaces that use `state` themselves
"(like bridge, ppp)" must have the esp_netif pointer in netif client data -
but only enables `LWIP_ESP_NETIF_DATA` when the bridge or PPP is built in.
Enabling the bridge is the cheapest way to flip it; the callback then resolves
to NULL for netifs esp_netif did not create and returns instead of following
a wild pointer.

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
- `include/esp_wireguard.h`, `src/esp_wireguard.c`, `src/wireguardif.c`: added
  the optional `esp_wireguard_trace` hook and `ESP_WG_TRACE()` calls through
  `esp_wireguard_connect()` and `wireguardif_init()`. The tunnel is brought up
  on the lwIP thread, so a fault in there takes the device down with nothing
  in any API to show for it; the hook lets the caller record how far it got in
  storage that survives a panic reset. Unset it is a NULL check and nothing
  more. This is what located the `netif->state` fault above.
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
- The tcpip task stack is enlarged to 8192
  (`CONFIG_LWIP_TCPIP_TASK_STACK_SIZE` in `sdkconfig.defaults`), because
  `psa_crypto_init()` runs there via the marshalled init and the component's
  `sys_timeout` rekey timer runs the x25519/blake2s handshake crypto there
  unconditionally. Measured headroom on hardware is comfortable - about
  1.3 KB of the 8 KB used at the deepest point, reported live as
  `wireguard.tcpip_stack_free_min` - so this is precaution, not a fix for
  anything observed.
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
