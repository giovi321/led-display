---
title: WiFi setup
description: Put the display on your network through the captive portal
sidebar:
  order: 4
---

A freshly flashed device has no WiFi credentials and starts its own access point.

1. The display boots, finds no stored credentials, and opens the network `LEDDisplay-Setup` (no password)
2. Connect to it from a phone or laptop. The captive portal opens by itself; if it does not, browse to `http://192.168.4.1/`
3. Scan for networks, pick yours, enter the password, save
4. The device reboots and joins your network in station mode

On the first boot after joining, the display scrolls its assigned IP address so you know where to point the browser. After that it shows whatever boot screen you configured (last text, IP, or nothing) in System, and the amber border splash always flashes for one second at power-on as a life sign.

If the network is unreachable for 10 seconds at boot (wrong password, moved house, AP down), the device falls back to the `LEDDisplay-Setup` portal so you can reconfigure it. Reset WiFi from the Network tab clears the stored credentials and reboots into the portal on purpose.

The SSID is limited to 32 characters and the password to 64, matching 802.11. The setup portal itself needs no API key even when one is configured; everything else does once you set one.
