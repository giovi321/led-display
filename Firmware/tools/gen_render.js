// Generates docs/src/assets/display-render.svg: a face-on render of the
// reference build, drawn from the real cut geometry in 3D/8x32/*.dxf and the
// firmware's own 5x7 font, so the lit pixels are the ones the device would
// actually light.
//
//   node Firmware/tools/gen_render.js Firmware/main/fonts.c > docs/src/assets/display-render.svg
//
// Geometry taken from the DXF (all millimetres). The cut file stores every
// curve as a polyline, so the holes below are the clustered loop centres, not
// CIRCLE entities: all twelve come out at r 2.500, i.e. 5 mm clearance holes
// for the M5 screws. The 5.25 radius that appears at (+-329.75, +-89.75) is
// the corner fillet of the sheet outline, not a thirteenth fixing.
//   sheet          670 x 190 overall, 5.25 corner radius
//   fixing holes   12 of 5 mm dia at (+-327, {-87, 0, +87}),
//                  (+-163.5, +-87) and (0, +-87)
//   wire cutouts   12 of 20 x 30 through the support panel
//   LED pitch      10, giving 640 x 160 of matrix inside a 15 mm border

const fs = require('fs');

// ---- the firmware's 5x7 font, read straight out of fonts.c ----------------
const src = fs.readFileSync(process.argv[2] || 'Firmware/main/fonts.c', 'utf8');
const rows = [];
for (const line of src.split('\n')) {
  const m = line.split('//')[0].trim().replace(/,$/, '')
    .match(/^\{(0x[0-9A-Fa-f]{2}(?:,0x[0-9A-Fa-f]{2}){4})\}$/);
  if (m) rows.push(m[1].split(',').map(v => parseInt(v, 16)));
}
if (rows.length !== 95) throw new Error('expected 95 glyphs, got ' + rows.length);

const FONT_W = 5, FONT_H = 7, DIGIT_W = 5, SPACE_W = 3;
const glyph = ch => {
  let c = ch.charCodeAt(0);
  if (c < 0x20 || c > 0x7e) c = 0x3f;
  return rows[c - 0x20];
};
function span(ch) {                      // proportional, tabular digits
  if (ch >= '0' && ch <= '9') return { start: 0, width: DIGIT_W };
  const g = glyph(ch);
  let first = -1, last = -1;
  for (let i = 0; i < FONT_W; i++) if (g[i]) { if (first < 0) first = i; last = i; }
  if (first < 0) return { start: 0, width: SPACE_W, blank: true };
  return { start: first, width: last - first + 1 };
}
function litPixels(text, scale, W, H) {
  let w = 0;
  for (const ch of text) w += (span(ch).width + 1) * scale;
  w -= scale;
  const x0 = Math.floor((W - w) / 2);
  const y0 = Math.floor((H - FONT_H * scale) / 2);
  const on = new Set();
  let cur = x0;
  for (const ch of text) {
    const s = span(ch);
    if (!s.blank) {
      const g = glyph(ch);
      for (let c = 0; c < s.width; c++) {
        const bits = g[s.start + c];
        for (let r = 0; r < FONT_H; r++) {
          if (!(bits & (1 << r))) continue;
          for (let dy = 0; dy < scale; dy++)
            for (let dx = 0; dx < scale; dx++)
              on.add((cur + c * scale + dx) + ',' + (y0 + r * scale + dy));
        }
      }
    }
    cur += (s.width + 1) * scale;
  }
  return on;
}

// ---- geometry -------------------------------------------------------------
const SHEET_W = 670, SHEET_H = 190, CORNER_R = 5.25;
const PITCH = 10, COLS = 64, ROWS = 16;
const MARGIN = (SHEET_W - COLS * PITCH) / 2;          // 15 mm all round

// Fixing holes, centred on the sheet, straight from the DXF. Twelve M5
// clearance holes, all the same size - the corner ones included.
const HOLE_R = 2.5;                                   // 5 mm dia, from the DXF
const HEAD_R = 4.4;                                   // M5 wafer head, dk 8.8 mm
const SOCKET_AF = 3;                                  // 3 mm hex key across flats
const HOLES = [
  ...[-327, 327].flatMap(x => [-87, 0, 87].map(y => ({ x, y }))),
  ...[-163.5, 0, 163.5].flatMap(x => [-87, 87].map(y => ({ x, y }))),
].map(h => ({ ...h, r: HOLE_R }));
if (HOLES.length !== 12) throw new Error('expected 12 fixings, got ' + HOLES.length);
const CUTOUT_X = [-290, -170, -50, 30, 150, 270];     // 20 x 30, two rows

const TEXT = '1234', SCALE = 2;
const lit = litPixels(TEXT, SCALE, COLS, ROWS);

const OX = 24, OY = 34;                               // sheet origin
const cx = i => OX + MARGIN + PITCH / 2 + i * PITCH;
const cy = j => OY + MARGIN + PITCH / 2 + j * PITCH;
const hx = h => OX + SHEET_W / 2 + h.x;
const hy = h => OY + SHEET_H / 2 + h.y;

let leds = '';
for (let j = 0; j < ROWS; j++) {
  for (let i = 0; i < COLS; i++) {
    const on = lit.has(i + ',' + j);
    leds += `<circle cx="${cx(i)}" cy="${cy(j)}" r="${on ? 3.6 : 3.1}" class="${on ? 'on' : 'off'}"/>`;
  }
  leds += '\n      ';
}

// Screws, face on: the wafer head covers an 8.8 mm circle over the 5 mm hole,
// and a socket screw shows a hex recess rather than a slot. The inner circle
// is the edge radius that rings the flat face, not a countersink cone: this
// head sits on the surface rather than in it. All twelve are the same M5x40,
// so all twelve draw identically.
const hexPath = (x, y, af) => {
  const rc = af / Math.sqrt(3);                       // across flats -> corners
  return Array.from({ length: 6 }, (_, i) => {
    const a = Math.PI / 6 + i * Math.PI / 3;
    return `${(x + rc * Math.cos(a)).toFixed(2)},${(y + rc * Math.sin(a)).toFixed(2)}`;
  }).join(' ');
};
const screw = (x, y) =>
  `<g class="screw"><circle cx="${x}" cy="${y}" r="${HEAD_R}" class="shead"/>` +
  `<circle cx="${x}" cy="${y}" r="${(HEAD_R - 0.5).toFixed(2)}" class="sface"/>` +
  `<polygon points="${hexPath(x, y, SOCKET_AF)}" class="ssocket"/></g>`;

const screwsFront = HOLES.map(h => screw(hx(h), hy(h))).join('');

// ---- section through the sandwich ----------------------------------------
// Layers sit at their real thicknesses, so the 6 mm short spacers and the
// 22 mm long ones read as different gaps. Only the vertical axis is
// exaggerated (x stays 1:1 with the front view); the caption says by how
// much, because the stack is 36 mm against a 670 mm sheet and would
// otherwise be a hairline.
const SY = 312;            // front face of the front panel
const VS = 3;              // mm -> px, vertical only
const SPACER_OD = 10;      // printed spacer outside diameter
const SHANK_D = 5;         // M5
const HEAD_H = 1.4;        // wafer head k, standing proud of the front panel
const NUT_AF = 8, NUT_HEX_H = 4, NUT_DOME_R = 6;   // DIN 1587 M5 dome nut: s 8, h 4, m 10
const NUT_THREAD = 4;      // il, the nut's threaded depth
const SCREW_L = 40;        // M5x40, measured under the head

const layers = [
  { t: 3,  cls: 'plexi', label: 'Front panel, 3 mm smoked grey plexiglass' },
  { t: 6,  cls: 'gap', spacer: true, label: 'Short spacers, 6 mm' },
  { t: 2,  cls: 'alu',   label: 'Aluminium support panel, 2 mm, LED panels glued on' },
  { t: 22, cls: 'gap', spacer: true, label: 'Long spacers, 22 mm, wiring and PCB' },
  { t: 3,  cls: 'plexi', label: 'Back panel, 3 mm smoked grey plexiglass' },
];

// Run the thicknesses up front: the screw and the labels both need the
// finished stack height, not just their own layer.
let acc = SY;
for (const l of layers) {
  l.y = acc;
  l.h = l.t * VS;
  acc += l.h;
}
const stackTop = SY, stackBot = acc;
const aluY = layers.find(l => l.cls === 'alu').y;

// One fixing per column: the cut plane is offset so it steps through a screw
// in each of the five columns while still crossing the LED panels.
const COLS_X = [...new Set(HOLES.map(h => h.x))].sort((a, b) => a - b);
const sx = x => OX + SHEET_W / 2 + x;

let stack = '', spacers = '';
for (const l of layers) {
  stack += `<rect x="${OX}" y="${l.y}" width="${SHEET_W}" height="${l.h}" class="${l.cls}"/>`;
  if (!l.spacer) continue;
  for (const x of COLS_X) {
    spacers += `<rect x="${(sx(x) - SPACER_OD / 2).toFixed(2)}" y="${l.y}" ` +
               `width="${SPACER_OD}" height="${l.h}" class="spacer"/>`;
  }
}

// LED panels on the front face of the support panel, and the wire cutouts
// through it.
let panels = '';
for (const p of [0, 1]) {
  panels += `<rect x="${OX + MARGIN + p * 320}" y="${(aluY - 3 * VS).toFixed(2)}" ` +
            `width="320" height="${3 * VS}" class="ledpanel"/>`;
}
let cuts = '';
for (const x of CUTOUT_X) {
  cuts += `<rect x="${OX + SHEET_W / 2 + x}" y="${aluY}" width="20" height="${2 * VS}" class="cut"/>`;
}

// The screw itself: a wafer head sitting on the front panel rather than sunk
// into it, shank down through the stack, and the dome nut closed on the thread
// that comes out of the back. The head is only 1.4 mm thick, which is the
// reason for the type: the laser cuts a plain 5 mm bore and there is no
// chamfer for a countersunk head to seat in. Length is measured under the
// head, so 40 mm over a 36 mm stack leaves 4 mm, exactly the nut's thread.
const tipY = SY + SCREW_L * VS;
let screwsSide = '';
for (const x of COLS_X) {
  const c = sx(x);
  const hw = HEAD_R, sw = SHANK_D / 2, nw = NUT_AF / 2, kw = SOCKET_AF / 2;
  screwsSide +=
    `<g class="screw">` +
    `<rect x="${c - sw}" y="${SY}" width="${SHANK_D}" ` +
    `height="${(tipY - SY).toFixed(2)}" class="shank"/>` +
    // wafer head: 8.8 mm across, 1.4 mm proud of the surface, hex recess
    `<rect x="${(c - hw).toFixed(2)}" y="${(SY - HEAD_H * VS).toFixed(2)}" ` +
    `width="${(HEAD_R * 2).toFixed(2)}" height="${(HEAD_H * VS).toFixed(2)}" ` +
    `rx="0.7" class="shead"/>` +
    `<rect x="${c - kw}" y="${(SY - HEAD_H * VS).toFixed(2)}" width="${SOCKET_AF}" ` +
    `height="${(HEAD_H * VS).toFixed(2)}" class="ssocket"/>` +
    // dome nut, seated on the back face, its 4 mm thread filled by the 4 mm
    // of shank that clears the stack
    `<rect x="${c - nw}" y="${stackBot}" width="${NUT_AF}" ` +
    `height="${NUT_HEX_H * VS}" class="nut"/>` +
    `<path d="M ${c - nw} ${stackBot + NUT_HEX_H * VS} ` +
    `a ${nw} ${NUT_DOME_R * VS} 0 0 0 ${NUT_AF} 0 z" class="nut"/>` +
    `</g>`;
}

// Labels on an even pitch with a leader back to their layer, so the 2 mm
// support panel gets a readable caption without crowding its neighbours.
// Only ~20 px separate the sheet edge from the label column, so the leader
// is a single straight run rather than an elbow.
const LBL_X = OX + SHEET_W + 22;
const lblY = i => stackTop + 6 + i * ((stackBot - stackTop) / (layers.length - 1));
let legend = '';
layers.forEach((l, i) => {
  const ly = lblY(i), my = l.y + l.h / 2;
  legend += `<path d="M ${OX + SHEET_W + 1} ${my.toFixed(2)} ` +
            `L ${LBL_X - 5} ${ly.toFixed(2)}" class="leader"/>` +
            `<text x="${LBL_X}" y="${(ly + 4).toFixed(2)}" class="lbl">${l.label}</text>`;
});

const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1050 490" width="1050" height="490" role="img" aria-label="Render of the assembled 64 by 16 LED display: four WS2812B 32x8 panels behind a smoked grey plexiglass front showing 1234, with a section through the layer stack">
  <title>led-display, reference build</title>
  <desc>Face-on view of the assembled display and a section through its layers, to the cut sizes in 3D/8x32: a 670 by 190 mm sheet with twelve M5 perimeter fixings, carrying a 64 by 16 pixel matrix on a 10 mm pitch. The section shows the five layers at their real thicknesses - 3 mm front panel, 6 mm short spacers, 2 mm aluminium support panel, 22 mm long spacers, 3 mm back panel - with a wafer head screw sitting on the front surface and a dome nut closed on the back.</desc>
  <defs>
    <linearGradient id="plexiFill" x1="0" y1="0" x2="0.35" y2="1">
      <stop offset="0" stop-color="#3a3f45"/>
      <stop offset="0.5" stop-color="#2a2e33"/>
      <stop offset="1" stop-color="#1f2226"/>
    </linearGradient>
    <linearGradient id="sheen" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0" stop-color="#ffffff" stop-opacity="0.10"/>
      <stop offset="0.45" stop-color="#ffffff" stop-opacity="0.02"/>
      <stop offset="1" stop-color="#ffffff" stop-opacity="0"/>
    </linearGradient>
    <filter id="glow" x="-120%" y="-120%" width="340%" height="340%">
      <feGaussianBlur stdDeviation="3.4" result="b"/>
      <feMerge><feMergeNode in="b"/><feMergeNode in="b"/><feMergeNode in="SourceGraphic"/></feMerge>
    </filter>
    <style>
      .off   { fill: #15171a; }
      .on    { fill: #ffb347; filter: url(#glow); }
      .plexi { fill: url(#plexiFill); stroke: #4a5058; stroke-width: 0.8; }
      .alu   { fill: #8a9099; stroke: #6d737b; stroke-width: 0.6; }
      .gap   { fill: none; stroke: #4a5058; stroke-width: 0.7; stroke-dasharray: 5 4; }
      .ledpanel { fill: #14161a; stroke: #2e3238; stroke-width: 0.5; }
      .cut   { fill: #23262b; }
      .shead { fill: #9aa1aa; stroke: #5f656d; stroke-width: 0.5; }
      .sface { fill: #a8afb8; }
      .ssocket { fill: #3f444b; }
      .shank { fill: #8a9099; stroke: #5f656d; stroke-width: 0.4; }
      .nut   { fill: #9aa1aa; stroke: #5f656d; stroke-width: 0.5; }
      .spacer{ fill: #3a3f45; stroke: #565c64; stroke-width: 0.5; }
      .leader{ fill: none; stroke: #5a6068; stroke-width: 0.6; }
      .lbl   { fill: #8f8e96; font: 12px system-ui, sans-serif; }
      .dimt  { fill: #8f8e96; font: 12px system-ui, sans-serif; }
      .diml  { stroke: #5a6068; stroke-width: 0.7; }
      .cap   { fill: #ecebe6; font: 600 14px system-ui, sans-serif; }
    </style>
  </defs>

  <rect width="1050" height="490" fill="#0b0b0d"/>

  <text x="${OX}" y="${OY - 12}" class="cap">Reference build, front</text>

  <rect x="${OX}" y="${OY}" width="${SHEET_W}" height="${SHEET_H}" rx="${CORNER_R}" class="plexi"/>
  <g>
      ${leds}
  </g>
  ${screwsFront}
  <rect x="${OX}" y="${OY}" width="${SHEET_W}" height="${SHEET_H}" rx="${CORNER_R}" fill="url(#sheen)" pointer-events="none"/>

  <line x1="${OX}" y1="${OY + SHEET_H + 16}" x2="${OX + SHEET_W}" y2="${OY + SHEET_H + 16}" class="diml"/>
  <text x="${OX + SHEET_W / 2}" y="${OY + SHEET_H + 32}" class="dimt" text-anchor="middle">670 &#215; 190 mm &#183; 64 &#215; 16 pixels &#183; 10 mm pitch &#183; 12 perimeter fixings</text>

  <text x="${OX + SHEET_W + 16}" y="${OY + 14}" class="cap">4 &#215; WS2812B 32&#215;8</text>
  <text x="${OX + SHEET_W + 16}" y="${OY + 34}" class="lbl">2 &#215; 2 grid, 1024 LEDs</text>
  <text x="${OX + SHEET_W + 16}" y="${OY + 52}" class="lbl">5 V, flexible panels</text>

  <text x="${OX}" y="${SY - 34}" class="cap">Section through the fixings</text>
  <text x="${OX}" y="${SY - 16}" class="lbl">Offset cut plane, stepped to take one screw per column &#183; vertical scale &#215;${VS}, horizontal 1:1</text>
  <g>${stack}</g>
  <g>${spacers}</g>
  ${panels}
  ${cuts}
  <g>${screwsSide}</g>
  <g>${legend}</g>

  <text x="${OX}" y="${(stackBot + NUT_HEX_H * VS + NUT_DOME_R * VS + 22).toFixed(2)}" class="dimt">M5&#215;40 ultra-thin wafer head socket screw from the front, M5 dome nut at the back &#183; 36 mm stack leaves 4 mm of thread, the nut's full depth</text>
</svg>
`;

process.stdout.write(svg);
