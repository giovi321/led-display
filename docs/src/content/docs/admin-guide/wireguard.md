---
title: WireGuard
description: An optional built-in WireGuard client for reaching the display from outside the LAN without exposing its plain-HTTP port to the internet
sidebar:
  order: 7
---

The firmware includes an optional WireGuard client. Its purpose is remote access done right: the device dials out to a WireGuard peer you control (a VPS, a home router, an existing VPN server) and becomes reachable through the encrypted tunnel. The alternative, port-forwarding the device's plain-HTTP port 80 to the internet, would expose an unauthenticated or at best [shared-key](../api-key/) HTTP API to the whole world. Do not do that.

The client is marked experimental until it has been proven against a live peer on hardware; the code builds and runs, but treat it as a feature to verify in your own setup rather than rely on sight unseen.

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
"status": {"compiled_in": true, "up": true, "last_handshake": 1755158400}
```

`up` means the peer currently answers handshakes; `last_handshake` is a Unix timestamp of the most recent completed handshake, 0 if none yet. The Network tab renders this as a one-line summary ("Tunnel up - last handshake 2 min ago").

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
