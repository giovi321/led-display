---
title: WireGuard
description: An optional built-in WireGuard client for reaching the display from outside the LAN without exposing its plain-HTTP port to the internet
sidebar:
  order: 7
---

The firmware includes an optional WireGuard client. Its purpose is remote access done right: the device dials out to a WireGuard peer you control (a VPS, a home router, an existing VPN server) and becomes reachable through the encrypted tunnel. The alternative, port-forwarding the device's plain-HTTP port 80 to the internet, would expose an unauthenticated or at best [shared-key](../api-key/) HTTP API to the whole world. Do not do that.

:::caution[Update to 2.3.1 or later]
On 2.2.0 and 2.3.0, enabling the tunnel panics the device the moment it is switched on, and it reboots until you disable it again. The cause and the fix are described in [why the tunnel used to crash](#why-the-tunnel-used-to-crash) below. If you are stuck on an affected build, power-cycle and disable the tunnel from the Network tab before it reconnects, then update.
:::

## The vendored component

The WireGuard implementation is a vendored copy of the `esp_wireguard` component, taken from the droscy fork (release 0.4.5) of the original trombik project, chosen because it already carried fixes for ESP-IDF 6 and mbedtls 4. It was then ported for this firmware; the full list of local changes, validation notes, and runtime caveats lives in `Firmware/components/esp_wireguard/PORTING.md` in the repository.

## Configuration

Network tab ([advanced mode](../advanced-mode/)), WireGuard card, or `POST /api/wireguard`. The fields:

| Field | Meaning |
|-------|---------|
| enabled | starts or stops the tunnel; all other fields must be filled before enabling |
| private_key | the device's own WireGuard private key; write-only, leaving it blank on save keeps the stored key |
| peer_public_key | public key of the peer the device connects to |
| endpoint_host, endpoint_port | where the peer listens; hostname or IP, default port 51820 |
| address | the device's IP inside the tunnel, CIDR notation; a bare IP is treated as /32 |
| allowed_ips | comma-separated IPv4 CIDRs routed through the tunnel; 0.0.0.0/0 makes the tunnel the default route |
| keepalive_s | persistent keepalive in seconds, default 25, 0 disables; keep it on when the device sits behind NAT |

The device does not generate keys. Create the pair on any machine with WireGuard tools and paste the private key into the form:

```bash
wg genkey | tee device.key | wg pubkey > device.pub
```

On the peer side, add the device as a `[Peer]` with the contents of `device.pub` and `AllowedIPs` set to the device's tunnel address as a /32.

The private key is stored in the device's flash and is included in the [config export](../backup-restore/); it never appears in `GET /api/wireguard` responses (only a `private_key_set` flag), status output, or logs.

## Status

`GET /api/wireguard` includes a status object, and the same fields appear in `/api/status`:

```json
"status": {"compiled_in": true, "up": true, "held": false, "last_handshake": 1755158400}
```

`up` means the peer currently answers handshakes; `last_handshake` is a Unix timestamp of the most recent completed handshake, 0 if none yet. The Network tab renders this as a one-line summary ("Tunnel up - last handshake 2 min ago").

## Why the tunnel used to crash

Worth writing down, because the symptom pointed everywhere except the cause.

ESP-IDF's networking layer, `esp_netif`, keeps its own handle for each interface in the lwIP `netif`'s `state` field, and the global callback it registers for interface events casts that field back to an `esp_netif` handle without checking. WireGuard does not go through `esp_netif`: it adds a raw lwIP interface whose `state` points at the component's own device structure. The first address event fired while that interface was being created therefore handed ESP-IDF a pointer to something that was never an `esp_netif`, it dereferenced it, and the device panicked - before a single line of WireGuard's own setup had run.

ESP-IDF documents the rule in `lwipopts.h`: interfaces that use `state` for themselves need the `esp_netif` handle kept in lwIP's per-interface client data instead. It only turns that on automatically for the two such interfaces it ships (the bridge and PPP), and a third-party interface like WireGuard is not on the list. The fix is to turn it on explicitly, which the firmware now does (`CONFIG_ESP_NETIF_BRIDGE_EN`, `CONFIG_LWIP_NUM_NETIF_CLIENT_DATA=1`); the callback then resolves to nothing for interfaces `esp_netif` did not create, and returns instead of following a wild pointer.

Two things this was mistaken for along the way, both wrong: the lwIP task's stack (the tunnel's crypto does run there, but measurement showed under 1.5 KB of a 8 KB stack in use) and the timing of NTP (the 90-second delay before the crash was just the tunnel waiting for the clock; once the network allowed NTP, the crash moved to immediately after switching the tunnel on). `/api/status` now reports `wireguard.tcpip_stack_free_min` so the stack question can be answered with a number rather than a guess.

A crash during bring-up also used to be able to roll back a firmware update: the tunnel started connecting before the new image had been marked valid, so the bootloader reverted a perfectly good update and the old version reappeared by itself. The tunnel and the MQTT client now start only after that point.

## The crash-loop guard

A tunnel bug must never brick the device out of its own web UI. The firmware counts consecutive crash reboots (panic or watchdog resets) that happen while the tunnel is enabled; after three in a row it holds the tunnel down at boot instead of starting it. The device then boots normally, the web UI stays reachable, and the status reports `held: true` ("Tunnel suspended after repeated reboots").

The counter resets on any clean boot (power cycle, reboot button, OTA restart) and on the first completed handshake. Re-saving the WireGuard config lifts the hold and retries immediately - that is the deliberate "I fixed it, try again" signal.

## How it behaves at runtime

The tunnel runs from a background task that never blocks boot. Connection attempts retry with a backoff that starts at 5 seconds and doubles up to 60 seconds, which covers WiFi still coming up, DNS not resolving yet, and an unreachable endpoint. If a connected tunnel completes no handshake for 180 seconds, the task tears it down and rebuilds it, which also re-resolves the endpoint hostname in case its DNS record changed.

WireGuard handshake timestamps require sane wall time, or the peer rejects the initiation as a replay. The task therefore waits up to 90 seconds for NTP sync before its first attempt, then proceeds regardless (a peer that has never seen this device accepts any initial timestamp).

## Limitations

- one tunnel, one peer; the component keeps its state in a single instance
- allowed IPs are IPv4 only
- needs NTP time before the handshake, as above; a network that blocks NTP delays or breaks the tunnel
- the endpoint hostname is resolved when the tunnel is built, not per packet; a moved endpoint is picked up by the 180 second stall recovery
- experimental until proven on hardware, per the note at the top

## Disabling at build time

WireGuard is compiled in by default but stays down until configured. To remove it from the image entirely, disable `CONFIG_LEDDISP_WIREGUARD` in `idf.py menuconfig` under the "LED display" menu. A build without it reports `compiled_in: false`, the Network tab shows "WireGuard is not included in this build", and `POST /api/wireguard` answers 501.
