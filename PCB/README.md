# Controller board, revision v3

Carrier board for the LED display: it sockets an ESP32-S3 dev board and adds the
level shifter, the series resistor, decoupling and the power-injection
connectors. Drawn in [Fritzing](https://fritzing.org/). Everything is
through-hole.

The [hardware guide](https://giovi321.github.io/led-display/getting-started/hardware/#the-controller-board)
describes the board for builders. This file covers the files themselves.

## Files

| File | What it is |
|---|---|
| `RGB display_v3.fzz` | Fritzing source, the only editable file here |
| `RGB display_v3.zip` | The Gerber set, zipped for upload to a fab |
| `Gerber files/` | The same Gerbers, unpacked |
| `RGB display_v3_pcb.png` | Fritzing's own PCB view, for reference |

Gerber layer map: `.gtl` / `.gbl` copper top and bottom, `.gts` / `.gbs`
soldermask, `.gto` / `.gbo` silkscreen, `.gm1` board outline, `_drill.txt`
Excellon drill, `_pnp.xy` pick and place.

## The board, from the files

| | | Source |
|---|---|---|
| Size | about 63.6 x 48.6 mm | `_contour.gm1`, confirmed by the sketch's board property |
| Layers | 2, both routed, both poured, one stitching via | `.fz`, and `G04 DOUBLE SIDED` in every Gerber |
| Holes | 90: 86 plated + 4 unplated 2.2 mm mounting | `_drill.txt` |
| Drill sizes | 0.4, 0.7, 0.813, 0.889, 0.965, 2.2 mm | `_drill.txt` |
| Parts | 1 IC, 1 resistor, 6 capacitors, 6 connectors | `.fz` and `_pnp.xy` |

The outline is stroked on its centreline with an 8 mil aperture, so the
finished size lands between 63.39 x 48.41 and 63.59 x 48.61 mm depending on how
the fab interprets it. That is a Fritzing export convention, not a fault.

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
npx --yes @tracespace/cli@4.2.8 --out . "Gerber files/"*.g* "Gerber files/RGB display_v3_drill.txt"
```

That writes `RGB display_v3_.top.svg` and `.bottom.svg`. Convert to PNG before
committing them: Fritzing exports the ground pour as thousands of hairline
strokes, so the SVG runs to a couple of megabytes while a 2000 px PNG is under
300 KB.
