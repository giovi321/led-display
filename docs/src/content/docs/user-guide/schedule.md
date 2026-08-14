---
title: Schedule
description: Turn the display off at night and back on in the morning, per weekday
sidebar:
  order: 2
---

The Schedule tab turns the display off outside working hours and back on when they start. Off means dark, not gone: the content, the settings, and the web UI all keep working, only the brightness changes.

Set a turn-on time, a turn-off time, tick the weekdays it should apply to, and enable the schedule. The device checks the schedule every 30 seconds, so switching happens within half a minute of the configured time.

## On and off times

The window between the on time and the off time is when the display is lit. Windows that cross midnight work: on at 22:00 and off at 06:00 keeps the display lit through the night. For such a window, the hours after midnight count against the following day's checkbox, so a Friday 22:00 to 06:00 window needs Saturday ticked to stay lit past midnight.

Days you leave unticked keep the display off for the whole day.

## Timezone handling

The device has no timezone database. When you save the schedule, the browser sends its own UTC offset in minutes along with the times, and the device applies that fixed offset to the UTC time it gets via NTP. The times you enter therefore mean what your wall clock says, as long as you save from a device in the display's timezone.

Two consequences of the offset being fixed:

- Daylight saving is not tracked. After a clock change, the schedule drifts by one hour until you open the Schedule tab and press Save again
- Saving from a browser in a different timezone stores that browser's offset, not the display's

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

## Interaction with the staleness guard

The [staleness guard](../value-updates/#the-staleness-guard) is paused while the schedule holds the display dark: no dimming, no corner marker, no stale text, and no `value_stale` webhook fires. The check resumes when the display turns back on.

With the dim action the panel is still visible, so the guard keeps running through the off window: a stale value halves whatever level the schedule allows, dim level included.

One edge case is handled for you: if the guard's dim action triggered before the off time, the display comes back at the halved level in the morning, not at full brightness, so a stale value never looks fresh.

## Related pages

- [Display](../display/): text, colour, and brightness
- [Value updates](../value-updates/): feeding the display and the staleness guard
