// Generates docs/src/assets/display-render.svg: a face-on render of the
// reference build, drawn from the real cut geometry in 3D/8x32/*.dxf and the
// firmware's own 5x7 font, so the lit pixels are the ones the device would
// actually light.
//
//   node Firmware/tools/gen_render.js Firmware/main/fonts.c > docs/src/assets/display-render.svg
//
// Geometry taken from the DXF (all millimetres):
//   sheet          670 x 190 overall, 5.25 corner radius
//   fixing holes   4 corner at (+-329.75, +-89.75) and 8 more at
//                  (+-327, 0), (0, +-87), (+-163.5, +-87)
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

// Fixing holes, centred on the sheet, straight from the DXF.
const HOLES = [
  ...[-329.75, 329.75].flatMap(x => [-89.75, 89.75].map(y => ({ x, y, r: 5.25 }))),
  { x: -327, y: 0, r: 2.5 }, { x: 327, y: 0, r: 2.5 },
  { x: 0, y: -87, r: 2.5 },  { x: 0, y: 87, r: 2.5 },
  { x: -163.5, y: -87, r: 2.5 }, { x: -163.5, y: 87, r: 2.5 },
  { x: 163.5, y: -87, r: 2.5 },  { x: 163.5, y: 87, r: 2.5 },
];
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

// Screws: a head with a slot, sized from the hole it sits in.
const screw = (x, y, r) =>
  `<g class="screw"><circle cx="${x}" cy="${y}" r="${r}" class="shead"/>` +
  `<circle cx="${x}" cy="${y}" r="${(r * 0.62).toFixed(2)}" class="sinner"/>` +
  `<line x1="${(x - r * 0.5).toFixed(2)}" y1="${y}" x2="${(x + r * 0.5).toFixed(2)}" y2="${y}" class="sslot"/></g>`;

const screwsFront = HOLES.map(h => screw(hx(h), hy(h), h.r)).join('');

// ---- section through the sandwich ----------------------------------------
// Generous row pitch so the labels never collide, and every label is on the
// same baseline as its own layer.
const SY = 300, ROW = 30;
const layers = [
  { t: 3,  cls: 'plexi', label: 'Front panel, 3 mm smoked grey plexiglass' },
  { t: 6,  cls: 'gap',   label: 'Short spacers' },
  { t: 2,  cls: 'alu',   label: 'Aluminium support panel, LED panels glued on' },
  { t: 22, cls: 'gap',   label: 'Long spacers, wiring and PCB' },
  { t: 3,  cls: 'plexi', label: 'Back panel, 3 mm smoked grey plexiglass' },
];

let stack = '', legend = '', aluY = 0;
layers.forEach((l, i) => {
  const y = SY + i * ROW;
  stack += `<rect x="${OX}" y="${y}" width="${SHEET_W}" height="${l.t}" class="${l.cls}"/>`;
  legend += `<text x="${OX + SHEET_W + 16}" y="${y + l.t / 2 + 4}" class="lbl">${l.label}</text>`;
  if (l.cls === 'alu') aluY = y;
});

// LED panels on the support layer, the wire cutouts through it, and the same
// twelve screws passing through the whole stack.
let panels = '';
for (const p of [0, 1]) {
  panels += `<rect x="${OX + MARGIN + p * 320}" y="${aluY - 3}" width="320" height="3" class="ledpanel"/>`;
}
let cuts = '';
for (const x of CUTOUT_X) {
  cuts += `<rect x="${OX + SHEET_W / 2 + x}" y="${aluY}" width="20" height="2" class="cut"/>`;
}
const stackTop = SY, stackBot = SY + (layers.length - 1) * ROW + 3;
let screwsSide = '';
for (const h of HOLES) {
  // one per distinct x, drawn as a shaft through every layer
  screwsSide += `<line x1="${hx(h)}" y1="${stackTop - 4}" x2="${hx(h)}" y2="${stackBot + 4}" class="shaft"/>` +
                `<circle cx="${hx(h)}" cy="${stackTop - 5}" r="${h.r * 0.55}" class="shead"/>`;
}

const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1050 470" width="1050" height="470" role="img" aria-label="Render of the assembled 64 by 16 LED display: four WS2812B 32x8 panels behind a smoked grey plexiglass front showing 1234, with a section through the layer stack">
  <title>led-display, reference build</title>
  <desc>Face-on view of the assembled display and a section through its layers, to the cut sizes in 3D/8x32: a 670 by 190 mm sheet with twelve perimeter fixings, carrying a 64 by 16 pixel matrix on a 10 mm pitch.</desc>
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
      .sinner{ fill: #7e858e; }
      .sslot { stroke: #4a5058; stroke-width: 1.1; stroke-linecap: round; }
      .shaft { stroke: #5f656d; stroke-width: 1.2; stroke-dasharray: 3 2; }
      .lbl   { fill: #8f8e96; font: 12px system-ui, sans-serif; }
      .dimt  { fill: #8f8e96; font: 12px system-ui, sans-serif; }
      .diml  { stroke: #5a6068; stroke-width: 0.7; }
      .cap   { fill: #ecebe6; font: 600 14px system-ui, sans-serif; }
    </style>
  </defs>

  <rect width="1050" height="470" fill="#0b0b0d"/>

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

  <text x="${OX}" y="${SY - 18}" class="cap">Section</text>
  ${screwsSide}
  <g>${stack}</g>
  ${panels}
  ${cuts}
  <g>${legend}</g>
</svg>
`;

process.stdout.write(svg);
