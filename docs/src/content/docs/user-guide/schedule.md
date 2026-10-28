---
title: Schedule
description: Turn the display off at night and back on in the morning, per weekday
sidebar:
  order: 2
---

The Schedule tab, at `/schedule` in the browser, turns the display off outside working hours and back on when they start. Off means dark, not gone: the content, the settings, and the web UI all keep working, only the brightness changes.

Set a turn-on time, a turn-off time, tick the weekdays it should apply to, and enable the schedule. The device checks the schedule every 30 seconds, so switching happens within half a minute of the configured time.

## On and off times

The window between the on time and the off time is when the display is lit. Windows that cross midnight work: on at 22:00 and off at 06:00 keeps the display lit through the night. For such a window, the hours after midnight count against the following day's checkbox, so a Friday 22:00 to 06:00 window needs Saturday ticked to stay lit past midnight.

## Days you leave unticked

"On the other days" decides what an unticked day means, because there are two reasonable answers and they are not the same schedule:

- **Apply the off-action all day** (the default) treats the day as one long off-window. This is how you keep a display dark over a weekend: untick Saturday and Sunday and they behave as if the on-time never arrived
- **Run normally, ignore the schedule** hands those days back. The panel stays at its usual brightness around the clock and only the ticked days follow the on and off times

The difference bites hardest with a single day ticked. With the first setting, ticking only Monday leaves the display dark for six days out of seven, which is correct if that is what you meant, surprising if you meant "the schedule only governs Mondays". With the second, the other six days are simply untouched.

## Timezone handling

The Timezone selector decides which wall clock the on/off times follow:

- **A zone name** such as `Europe/Zurich` or `America/New_York`, the [IANA](https://www.iana.org/time-zones) names every operating system uses. The device stores that zone's daylight-saving rule and applies it itself, so the schedule stays right across clock changes with nothing to re-save. The list comes from your browser, and the zone your browser is in is marked, so picking the right one is usually one click
- **Fixed offset from this browser**, the fallback, copies the browser's current UTC offset when you press Save and applies it to the NTP time. It does not follow daylight saving: after a clock change the schedule drifts by an hour until you save again, and saving from a browser in another timezone stores that browser's offset

The device carries a compact table mapping each zone name to its rule, because it has no room for the full timezone database. If you set a zone name the device does not recognise, whether through the API or from a browser newer than the firmware, `GET /api/schedule` reports `tz_known: false`, the UI says so, and the schedule falls back to UTC until you pick a zone it knows. A raw [POSIX TZ string](https://www.gnu.org/software/libc/manual/html_node/TZ-Variable.html) such as `CET-1CEST,M3.5.0,M10.5.0/3` is also accepted and used as-is, which is the escape hatch for a zone whose rule changed after your firmware was built.

Time itself comes from NTP (`pool.ntp.org`) over the network; the sync starts at boot. Until the first successful sync the schedule does nothing and the display stays in whatever state it is in. No internet access means no schedule.

## Off or dim

"Outside those hours" decides what off means:

- Turn off (the default) sets the brightness to 0. The panel goes dark
- Dim to holds the panel at a level you choose, 1 to 255. Use it where the display should stay readable overnight but not light up the room

The dim level is a brightness like any other, so the [brightness cap](../display/#brightness) limits it too: the slider for it never goes above the cap.

## What off means

At the off time the device changes the brightness and nothing else. The frame content stays in memory, the stored settings are untouched, and the HTTP server keeps answering. In particular:

- [Value updates](../value-updates/) posted while the display is off are accepted, formatted, and stored as usual; you see the latest value the moment the display turns back on
- Changing the brightness during an off window stores the new level without lighting the panel. It takes effect at the next turn-on
- At the on time the device restores the brightness you last saved, or the level the [light sensor](../../admin-guide/light-sensor/) reads if you have one enabled

Disabling the schedule turns the display back on within one 30-second tick.

The `schedule_on` and `schedule_off` transitions fire whichever action is configured: consumers of the [outbound webhooks](../value-updates/#consuming-the-devices-webhooks) hear about the window, not about the brightness.

While the device runs its own [setup network](../../getting-started/wifi-setup/), the schedule never blanks the panel. The banner with the setup SSID and address is then the only way to find the device, so it stays visible until the device is back on a network.

## Interaction with the fallback

While the schedule holds the display dark, the [fallback](../value-updates/#fallback) has nothing to show: no corner marker is drawn and the dim has no effect on a panel that is off. Its triggers and notifications keep running, so a `value_stale` webhook can fire during the off window and a feed that dies overnight is reported when it dies, not when the display comes back.

With the dim action the panel is still visible, so the fallback keeps working through the off window: the fallback dim halves whatever level the schedule allows, dim level included.

One edge case is handled for you: if the fallback dim engaged before the off time, the display comes back at the halved level in the morning, not at full brightness, so a broken feed never looks healthy.

## Related pages

- [Display](../display/): text, colour, and brightness
- [Value updates](../value-updates/): feeding the display and the fallback
