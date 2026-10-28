---
title: Advanced mode
description: What the Advanced toggle reveals and why the web UI splits daily controls from one-off hardware and infrastructure settings
sidebar:
  order: 1
---

The web UI starts in simple mode: two tabs, Display and Schedule. That is everything needed day to day, which means changing the text, the colour, the brightness, and the on/off times. The Advanced toggle in the header reveals four more tabs: Hardware, Network, Integrations, and System.

The split follows how the settings are used. Display and Schedule get touched regularly. The advanced tabs hold things you configure once when setting the device up (which panel sits where, WiFi credentials, an API key) and then rarely revisit. Hiding them keeps the daily UI small enough to use comfortably from a phone, and keeps a stray tap from landing on Factory reset.

## What each tab holds

Simple mode:

- Display: text, colour picker with saved-colour chips, text size and spacing, brightness, scroll mode and speed
- Schedule: on/off times per weekday, the off-window action, synced to your browser's timezone

Advanced mode adds:

- Hardware: the [arrangement editor](../arrangement-editor/), test patterns, the brightness cap that protects the power supply, and the [light sensor](../light-sensor/)
- Network: the saved WiFi networks, scan and connect, and the [WireGuard](../wireguard/) tunnel
- Integrations: the [API key](../api-key/), the value sources (URL polling, the value endpoint and MQTT), the value formatting and fallback, and the status webhook
- System: firmware and status info, the device name, boot screen, [OTA updates](../ota-updates/), [backup and restore](../backup-restore/), reboot, and factory reset

## Behaviour details

The toggle state lives in your browser's localStorage, so it is remembered per browser, not per device. Another phone or laptop opening the same device starts in simple mode.

Switching advanced mode off while an advanced tab is open jumps back to the Display tab.

The toggle is a UI convenience, not access control. All API endpoints stay reachable regardless of the toggle. If you want mutating requests gated, set an [API key](../api-key/).
