---
title: Screens
description: What the panel shows at power-on, once the network is up, and when the WiFi link drops
sidebar:
  order: 4
---

Three separate settings decide what the panel shows at moments when the answer is not simply "your content": the second after power-on, the point where the network state becomes known, and a lost WiFi link. They live on the System tab, at `/system` in the browser, behind the [advanced mode](../../admin-guide/advanced-mode/) toggle.

They are independent. A device can flash its border test, settle on the IP address, hand over to the content five seconds later, and mark a corner pixel when the link drops, without any of those choices constraining the others.

## The startup splash

What the panel does before the network is up, while the device is still bringing itself round.

- **Border test** (the default) lights every edge pixel in your stored colour for one second. That default is deliberate: a panel that is miswired, half dead, or chained in the wrong order says so within a second of power-on, before any content can mask it. It is the cheapest hardware check you will ever run, and it runs every time
- **Blank** stays dark until the after-boot screen takes over
- **Content** skips the splash and goes straight to your content

## The boot screen

What the panel shows once the network state is known.

- **Content** (the default) shows what you configured. Two things happen automatically underneath it. In [integration mode](../value-updates/) the last value does not survive a reboot, so the [fallback](../value-updates/#fallback) text stands in until the next value arrives, and with no fallback text set the panel stays blank rather than letting an old static text pose as live data. In static mode on a first boot, where no text has been saved yet, it falls back to the IP address so you can find the web UI
- **IP address** scrolls the address the device was given, over and over
- **Nothing** leaves the panel blank
- **IP address, then content** shows the address for five seconds and then hands over. If you set something from the web UI during those five seconds, the handover is cancelled rather than overwriting what you just did

One override sits above all four. While the device runs its own [setup network](../../getting-started/wifi-setup/), the panel scrolls `AP mode, SSID: <name>, IP: <address>` instead, because the panel is then the only place that address appears. A boot screen of "Nothing" on an unreachable device would leave you with nothing to type into a browser.

## When WiFi is lost

What happens when the link to your network drops while the device keeps running. This is the only one of the three that has to decide between telling you something and preserving what you were reading.

- **Keep showing the last content** (the default) changes nothing. The panel goes on showing whatever it showed, and you find out from somewhere else
- **Show a message** replaces the content with up to 63 characters of your choosing, in the current colour and scroll settings
- **Keep the content and mark one pixel** leaves the display alone and lights a single pixel on top of it
- **Blank the panel** goes dark

### The marker pixel

The marker is an overlay, not a screen. The content underneath is untouched, so a scrolling value carries on scrolling and a static number stays exactly where it was; one pixel is stamped over the outgoing frame on its way to the panel. When the link comes back the pixel is removed and nothing is repainted, so the display never flickers back through its boot screen to tell you the network returned.

You choose its position as an X and Y coordinate anywhere on the panel, its colour, and its brightness. That brightness is its own: it is applied instead of the panel brightness and instead of the [power-budget cap](../display/#brightness), which is the point. An indicator that vanished when the [light sensor](../../admin-guide/light-sensor/) dimmed the room, or when the [schedule](../schedule/) turned the panel off for the night, would be an indicator you could not rely on. Set it low if a lit pixel at night would bother you; it is one LED.

The coordinates are checked against the panel you actually have, so the fields cannot offer a position that would land off the edge. Change the [arrangement](../../admin-guide/arrangement-editor/) to a smaller grid and a marker left outside it simply does not draw.

### What it does not cover

The setup network is deliberately exempt. When the device raises its own AP, the banner with the setup address wins over all four choices, marker included, for the same reason it beats the boot screen: that address is the only way back in.

## Related pages

- [Display](../display/): the content these screens interrupt, and the brightness they work against
- [Schedule](../schedule/): the other thing that decides whether the panel is lit
- [WiFi setup](../../getting-started/wifi-setup/): the setup network that overrides all of this
