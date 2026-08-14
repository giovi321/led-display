---
title: WiFi setup
description: Put the display on your network through the captive portal, and keep up to five networks stored for later
sidebar:
  order: 5
---

A freshly flashed device has no WiFi credentials and starts its own access point.

1. The display boots, finds no stored credentials, and opens the network `LEDDisplay-Setup` (no password). The panels scroll `Setup AP: LEDDisplay-Setup / 192.168.4.1` so you can read both from across the room
2. Connect to it from a phone or laptop. The captive portal opens by itself; if it does not, browse to `http://192.168.4.1/`
3. Scan for networks, pick yours, enter the password, save
4. The device joins your network in station mode. "Add and connect now" reboots straight onto it; plain "Add network" joins within moments without a reboot, and the setup network disappears once it is on

On the first boot after joining, the display scrolls its assigned IP address so you know where to point the browser. After that it shows whatever boot screen you configured (last text, IP, or nothing) in System, and the amber border splash always flashes for one second at power-on as a life sign.

## Several networks

The device stores up to five networks. Keeping a phone hotspot or a second office SSID next to the main one means a move, a router swap, or an outage does not send you back to the setup portal. A device updated from a firmware that only kept one network adopts those credentials as the first entry, so nothing has to be re-entered after the update.

The Network tab ([advanced mode](../../admin-guide/advanced-mode/)) lists the saved networks and has two buttons for adding one:

- Add network stores the credentials. An online display keeps the connection it has, so this is how a fallback gets added without interrupting anything; a display that is offline, including one sitting on its setup network, tries the new entry within moments and drops the setup network as soon as it joins
- Add and connect now stores them and reboots straight onto that network

Scan results mark the networks the device already knows, so you do not have to remember which ones are stored or retype a password for them. Adding an SSID that is already stored replaces its password. When all five slots are full, adding another is refused rather than silently dropping one: press Forget on the entry you no longer need first.

Forget all networks clears every slot and reboots into the setup portal on purpose.

Passwords are write-only. No endpoint and no [config backup](../../admin-guide/backup-restore/) ever returns one, which is also why a restored backup does not carry credentials.

## What happens at boot

The device scans first, then works through the saved networks in the order that is most likely to succeed:

1. every saved network the scan saw, strongest signal first
2. the ones the scan missed, in the order they were stored, so a hidden SSID still gets an attempt

Each attempt gets 10 seconds. If none of them answers, the device raises the `LEDDisplay-Setup` portal right away so you can reconfigure it, keeping the saved credentials. The portal and the station side run at the same time, which has two consequences worth knowing: the Scan button works while you are on the portal, and the device keeps trying the saved networks underneath it. A router that boots more slowly than the display therefore fixes itself, without you touching anything.

## When the network drops later

A link lost while running does not put the device into setup mode straight away. It retries the saved networks forever, waiting 2, 5, 15, 30 and then 60 seconds between passes.

If it is still offline after 30 minutes, it raises the setup network alongside those retries, so the portal is there when you come looking. Retries only slow to one every 5 minutes while somebody is actually connected to the portal, because a scan stalls the page. As soon as one of the saved networks answers, the setup network disappears again.

While the setup network is up, the panels scroll the setup SSID and address whatever the boot screen setting says, and neither the [schedule](../../user-guide/schedule/) nor the staleness guard nor the [light sensor](../../admin-guide/light-sensor/) may dim that banner away. A display nobody can reach still has to say how to reach it.

## Names and limits

The setup network is called `LEDDisplay-Setup` and the device answers to the hostname `led-display` until you set a [device name](../../api-reference/system/#post-apisystemdevice_name) in the System tab. That one name then replaces both, and shows up in outbound webhooks as well. Renaming does not disconnect you: the setup network is renamed the next time it appears.

Either name works as a URL: `http://led-display/` or `http://<your-name>/`, with a suffix such as `.local` or `.lan` if your router adds one. Writes from that URL are accepted like writes to the IP address. A name you set on an older firmware through the Integrations tab is adopted once on upgrade, unless it amounts to the default name after cleaning (spaces become hyphens, illegal characters go), in which case the device stays unnamed and keeps the default hostname and setup SSID.

The SSID is limited to 32 characters and the password to 64, matching 802.11. The WiFi endpoints need no API key on the setup network of a device that has nothing saved yet, which is the first-time case. Once networks are stored, a key applies everywhere, the fallback portal included, so keep the key: without it, WiFi cannot be reconfigured from the portal and the way back is a USB reflash.
