---
title: API key and web UI login
description: "Two optional locks, a shared key that gates every mutating request and a browser login over the whole interface: what they protect against, and what they deliberately do not"
sidebar:
  order: 4
---

The device speaks plain HTTP and is designed for a trusted LAN. By default there is no authentication at all: anyone who can reach port 80 can change the text, reflash the firmware, or factory reset the device.

There are two independent locks, both optional and both off out of the box. The **API key** gates every mutating request and is meant for scripts. The **web UI login** puts a browser username and password over the whole interface and is meant for people. They compose: with both on, a script sends the key and the credentials, and a browser is asked to log in and then needs the key too for writes.

## Threat model, stated plainly

The key is a single shared secret sent as a cleartext header over unencrypted HTTP. Anyone who can sniff your LAN traffic can read it, and anyone with the key can do anything the API allows. It stops casual misuse, meaning flatmates, curious guests on the WiFi, and your own automation hitting the wrong device. It does not defend against a hostile network.

If you want to reach the display from outside your LAN, do not port-forward the device. Use the built-in [WireGuard client](../wireguard/) so the device stays unreachable from the internet and remote traffic arrives through an encrypted tunnel.

## What the key gates

With a key set:

- every POST endpoint requires it: display, layout, schedule, system, OTA, WiFi, integrations, value, value preview, URL poll, WireGuard, config import
- `GET /api/config/export` requires it too, because the export contains the key itself, the poll and webhook secrets and the WireGuard private key
- all other GET endpoints stay open (status, layout, schedule, OTA status)
- one exception: on the setup AP of a device that has no saved network, the WiFi endpoints skip the check. That is genuine first-time provisioning, where nobody could have set a key yet

The exception is narrow on purpose. A device that already has saved networks and fell back to the setup AP after an outage still requires the key on WiFi POSTs: that AP is open, so a wider exception would let anyone within radio range move the display onto their own network or wipe its credentials.

The cost of that is worth stating plainly: if you set a key and then lose it, you cannot reconfigure WiFi from the fallback portal either, and the way back is a USB reflash. Keep the key somewhere you will still have it when the display is the thing that is broken.

With no key set, everything is open. That is the deliberate LAN-trust default.

Requests that fail the check get `401 {"ok":false,"error":"missing or wrong X-Api-Key"}`.

## Cross-site requests

Independently of the key, a POST has to look like it was meant for this device. The `Host` header, and the `Origin` header when a browser sends one, must name the device's IP, its WireGuard address, or its device name (optionally with one suffix label such as `.local`, and an optional port). Anything else is refused with `403 {"ok":false,"error":"cross-origin request rejected"}`.

That is what stops a random web page in your browser from posting to the display, and stops DNS rebinding from reading responses back, in the default configuration where no key is set. Browsing to `http://<device-name>/` is a documented way in, so writes from that URL pass. The check is skipped while the device runs its own setup AP, where the captive-portal sheet is served under whichever probe hostname the phone asked for.

## Setting and clearing the key

In the web UI: [advanced mode](../advanced-mode/), Integrations tab, API key card. Enter a key (up to 64 characters) and press Set key. Clear key removes it, after which all requests are allowed again.

Via the API:

```bash
# set
curl -X POST http://<device-ip>/api/integrations -d '{"api_key":"my-secret"}'

# clear (needs the current key once one is set)
curl -X POST http://<device-ip>/api/integrations -H 'X-Api-Key: my-secret' -d '{"api_key":""}'
```

`GET /api/integrations` never returns the key; it reports `"api_key_set": true` or `false`. The only way to read the key back off the device is the auth-gated [config export](../backup-restore/).

## Using the key

Send it as the `X-Api-Key` header on every request that needs it:

```bash
curl -X POST http://<device-ip>/api/display/text \
  -H 'X-Api-Key: my-secret' \
  -d '{"text":"42.5"}'
```

## The web UI and the key

The moment you press Set key, the same browser adopts the key and keeps working without interruption. Any other browser gets a prompt on its first rejected request; the entered key is remembered and the request retried.

The UI stores the key in the browser's localStorage. That means the key is required again after setting it, even in the UI, on any browser that has not stored it yet, and it means anyone with access to a logged-in browser profile can read the key from localStorage. Keep that in mind on shared machines.

## Web UI login

The API key answers "may this request change something". It says nothing to a browser that simply loads the page, so with a key set and no login, anyone on the network can still read your configuration, your WiFi networks, your webhook URL and your schedule. The web UI login closes that: a username and password, asked by the browser itself, over everything the device serves.

Turn it on in [advanced mode](../advanced-mode/), System tab, Web UI login card: tick the box, set a username and password, press Save login. It sits with the other device-wide settings rather than with the integrations, because it governs the whole interface and not one of them. Your browser asks for them on the next page load. Leaving the password box empty on a later save keeps the stored one, so you can rename the user or toggle the login without retyping it.

Via the API:

```bash
# turn on
curl -X POST http://<device-ip>/api/integrations \
  -d '{"ui_auth":true,"ui_user":"admin","ui_pass":"secret"}'

# turn off (needs the credentials, since the login now covers this endpoint)
curl -u admin:secret -X POST http://<device-ip>/api/integrations -d '{"ui_auth":false}'
```

Enabling it without a stored password is refused with `400 {"ok":false,"error":"ui_auth needs a ui_pass"}` rather than quietly doing nothing.

### What it covers, and what it does not

It covers every URI the device serves: the pages, the assets, and all of the API including the open GETs. A login that left `GET /api/status` readable would not be one.

It is HTTP Basic over plain HTTP. The credentials are base64, not encrypted, and they go out with every single request. This keeps casual visitors on your network out of the interface. It is no defence against anyone who can watch the traffic. For that, reach the device through the [WireGuard tunnel](../wireguard/) rather than exposing it.

It is not applied on the setup AP. In that mode the panel carries the only copy of the setup address, and the captive-portal probes have to answer normally rather than with a 401. So a device that has fallen back to its setup network is reachable without the login.

That last point is the recovery path, and it cuts both ways. There is no reset button on the board, so if you lose the password the way back is to stop the device reaching its network: turn the access point off, change its password, or carry the display out of range. After 30 minutes offline it raises its own setup AP alongside the retries, and that AP is not gated. Slow, but it beats a USB reflash, which is what a lost API key costs you.

The same door is open to anyone within radio range who can take your WiFi down, so treat the login as a lock on the front door of a trusted network, not as a boundary against a hostile one.
