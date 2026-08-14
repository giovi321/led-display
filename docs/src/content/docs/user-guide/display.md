---
title: Display
description: Text, colour, brightness, scrolling, and font size from the Display tab
sidebar:
  order: 1
---

The Display tab is the first tab of the web UI and controls what the display shows: the text, its colour, the brightness, whether it scrolls, and how big the font is. Everything you apply here is saved to flash and comes back after a power cycle.

Press Apply to send the current text, colour, scroll setting, speed, font size, and brightness to the device in one go. Clear display blanks the display and erases the stored text; all other settings stay.

## Text

Type the text and press Apply. The input accepts up to 120 characters (the API accepts up to 127, see the [API reference](../../api-reference/)). The font covers printable ASCII (characters 32 to 126); anything outside that range renders as `?`.

Static text is centered horizontally. If the text is wider than the display, enable scrolling, otherwise the right-hand part is cut off.

## Colour

The colour picker sets the text colour. Applying a colour does not interrupt the text: picking a saved colour chip changes the colour of what is already showing without re-sending the text.

### Saved colour chips

Chips are colour presets stored on the device itself, so every browser and phone that opens the UI sees the same list. The list holds at most 12 entries.

- Add: press the `+` chip to save the current picker colour. You are asked for a name (up to 16 characters); leaving it empty uses the hex code
- Apply: tap a chip. The displayed text switches to that colour immediately
- Remove: press the small `x` on the chip, or long-press the chip

## Brightness

The slider runs from 0 to 255 and takes effect when you press Apply. 0 is dark, 255 is full power.

The slider's maximum can be lower than 255: the brightness cap, set on the Hardware tab in advanced mode, limits how bright the display may go to protect the power supply. The slider never goes above the cap, and API requests asking for more are silently clamped to it rather than rejected. See the [admin guide](../../admin-guide/) for the cap itself.

The display can also dim or go dark on its own: the [schedule](../schedule/) turns it off outside the configured hours, and the [staleness guard](../value-updates/#the-staleness-guard) can halve the brightness when a value stops updating.

## Scrolling

Tick Scroll to make the text run across the display. Scrolling text enters from the right edge and moves left; when it has fully left the display it re-enters from the right.

The speed slider runs from 0 to 255. It maps to the time each one-pixel step takes: about 200 ms per step at 0 and about 15 ms per step at 255. The slider only matters while scrolling is enabled.

## Font size

The font is a 5x7 pixel font that the firmware enlarges by an integer factor: Small is the raw 5x7, Medium doubles it to 10x14, Large triples it to 15x21. Glyphs are centered vertically.

The size is clamped to what fits the display height, so a factor is never applied that would clip the top and bottom rows. A 7-pixel-tall glyph at factor N needs N x 7 rows: a 16-pixel-tall layout fits at most Medium, an 8-pixel-tall single panel only Small. Choosing Large on a display that cannot fit it silently falls back to the largest size that fits. The API accepts factors 1 to 8 under the same clamp.

The default after a reset depends on the layout height: Large on displays 21 pixels or taller, Medium at 14 or taller, Small below that.

## Related pages

- [Schedule](../schedule/): turn the display off at night
- [Value updates](../value-updates/): let another system set the text
