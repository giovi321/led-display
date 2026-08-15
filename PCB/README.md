# Controller board, revision v4

Carrier board for the LED display: it sockets an ESP32-S3 dev board and adds
the level shifter, the series resistor, decoupling, the power-injection
connectors and the ambient-light sensor connector. Drawn in
[Fritzing](https://fritzing.org/). Everything is through-hole.

The [hardware guide](https://giovi321.github.io/led-display/getting-started/hardware/#the-controller-board)
describes the board for builders. This file covers the files themselves.

## What changed from v3

One addition: a 4-way `GND SDA SCL 5V` connector for a BH1750 digital lux
sensor, which the firmware supports from 2.9.0 alongside the analog input it
has always had. It takes the board from six connectors to seven and from 90
drilled holes to 94. Nothing else moved: same outline, same layer count, same
parts otherwise.

## Files

| File | What it is |
|---|---|
| `RGB display_v4.fzz` | Fritzing source, the only editable file here |
| `Gerber.zip` | The Gerber set, zipped for upload to a fab |
| `Gerber/` | The same Gerbers, unpacked |

Gerber layer map: `.gtl` / `.gbl` copper top and bottom, `.gts` / `.gbs`
soldermask, `.gto` / `.gbo` silkscreen, `.gm1` board outline, `_drill.txt`
Excellon drill, `_pnp.xy` pick and place.

## The board, from the files

| | | Source |
|---|---|---|
| Size | about 63.6 x 48.6 mm | `_contour.gm1`, confirmed by the sketch's board property |
| Layers | 2, both routed, both poured, one stitching via | `.fz`, and `G04 DOUBLE SIDED` in every Gerber |
| Holes | 94: 90 plated + 4 unplated 2.2 mm mounting | `_drill.txt` |
| Drill sizes | 0.4, 0.7, 0.813, 0.889, 0.965, 2.2 mm | `_drill.txt` |
| Parts | 1 IC, 1 resistor, 6 capacitors, 7 connectors | `.fz` |

The 0.965 mm tool accounts for 61 of those holes: 44 for the two 22-way ESP32
sockets and 17 for the seven connectors. The outline is stroked on its
centreline with an 8 mil aperture, so the finished size lands between
63.39 x 48.41 and 63.59 x 48.61 mm depending on how the fab interprets it.
That is a Fritzing export convention, not a fault.

## Which header positions the nets use

The ESP32 sockets are generic 22-way parts, so the sketch records positions
rather than GPIO names. Against the
[ESP32-S3-DevKitC-1 J1 pinout](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.1.html),
with the dev board's USB ports facing right, they resolve as:

| Sketch position | J1 pin | Signal |
|---|---|---|
| `connector8` | 9 | GPIO16, LED data into the buffer |
| `connector11` | 12 | GPIO8, SDA |
| `connector14` | 15 | GPIO9, SCL |
| `connector20` | 21 | 5V into the dev board |

Four independent nets landing on four correct J1 pins is what confirms the
mapping; the firmware's `LED_DATA_GPIO 16` is the anchor.

## Known quirks

- **Capacitor values are not in the design.** C1 is a polarised radial on
  2.54 mm lead pitch, C2 to C6 non-polarised on 5 mm. The values are blank in
  the sketch. Nothing is marked do-not-populate
- **`_pnp.xy` says `SMT` for C1 to C6.** It is wrong: all six have plated
  0.7 mm drill holes. Every part on this board is through-hole
- **The silkscreen says `74AHCT125`.** That is the name of the Fritzing part
  the footprint came from. The part to fit is the SN74HCT125N; same DIP-14
  pinout
- **Some top silkscreen text is mirrored.** The `ESP32-S3-N16R8` label and the
  `C2` to `C5` designators come out backwards in the export while the rest of
  the text does not. Cosmetic, but fix it before any respin

## Rendering the board

The images in the documentation are generated from the Gerbers rather than
screenshotted from the editor, so they show what a fab would make:

```sh
npx --yes @tracespace/cli@4.2.8 --out . "Gerber/"*.g* "Gerber/RGB display_v4_drill.txt"
```

That writes `RGB display_v4_.top.svg` and `.bottom.svg`. Convert to PNG before
committing them: Fritzing exports the ground pour as thousands of hairline
strokes, so the SVG runs to a couple of megabytes while a 2000 px PNG is around
300 KB.
