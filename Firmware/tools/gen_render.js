// Generates docs/src/assets/display-render.svg: a face-on render of the
// reference build, drawn from the real cut dimensions in 3D/8x32/*.dxf and
// the firmware's own 5x7 font, so the lit pixels are the ones the device
// would actually light.
//
//   node gen_render.js > display-render.svg

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

// Which LEDs a string lights, at an integer scale, centred.
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

// ---- geometry, in millimetres, from 3D/8x32/*.dxf -------------------------
const SHEET_W = 660, SHEET_H = 180;   // front / back plexiglass and support panel
const PITCH = 10;                     // WS2812B 32x8 flexible panel, 10 mm pitch
const COLS = 64, ROWS = 16;           // 2x2 grid of 32x8 panels
const MARGIN = (SHEET_W - COLS * PITCH) / 2;   // 10 mm, and the same vertically

const TEXT = '1234';
const SCALE = 2;
const lit = litPixels(TEXT, SCALE, COLS, ROWS);

const OX = 20, OY = 20;               // sheet origin inside the drawing
const cx = i => OX + MARGIN + PITCH / 2 + i * PITCH;
const cy = j => OY + MARGIN + PITCH / 2 + j * PITCH;

let leds = '';
for (let j = 0; j < ROWS; j++) {
  for (let i = 0; i < COLS; i++) {
    const on = lit.has(i + ',' + j);
    leds += `<circle cx="${cx(i)}" cy="${cy(j)}" r="${on ? 3.6 : 3.1}" ` +
            `class="${on ? 'on' : 'off'}"/>`;
  }
  leds += '\n    ';
}

// ---- the layer stack, drawn as a side elevation ---------------------------
const LY = 250;                        // top of the section drawing
const layers = [
  { t: 3,  cls: 'plexi',  label: 'Front panel, smoked grey plexiglass' },
  { t: 6,  cls: 'gap',    label: 'Short spacers' },
  { t: 2,  cls: 'alu',    label: 'Aluminium support panel, LED panels glued on' },
  { t: 22, cls: 'gap',    label: 'Long spacers, wiring and PCB' },
  { t: 3,  cls: 'plexi',  label: 'Back panel, smoked grey plexiglass' },
];
let stack = '', y = LY, legend = '';
layers.forEach((l, i) => {
  stack += `<rect x="${OX}" y="${y}" width="${SHEET_W}" height="${l.t}" class="${l.cls}"/>\n    `;
  legend += `<text x="${OX + SHEET_W + 14}" y="${y + l.t / 2 + 3.5}" class="lbl">${l.label}</text>\n    `;
  y += l.t + 2;
});
const stackH = y - LY;

// LED panels sitting on the support layer, and the wire cutouts through it.
const aluY = LY + 3 + 2 + 6 + 2;
let panels = '';
for (const px of [0, 1]) {
  panels += `<rect x="${OX + MARGIN + px * 320}" y="${aluY - 2.5}" width="320" height="2.5" class="ledpanel"/>`;
}
// cutouts at the x positions found in the DXF, relative to a centred sheet
const CUTS = [-290, -170, -50, 30, 150, 270];
let cuts = '';
for (const x of CUTS) {
  cuts += `<rect x="${OX + SHEET_W / 2 + x}" y="${aluY}" width="20" height="2" class="cut"/>`;
}

const svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1010 330" width="1010" height="330" role="img" aria-label="Render of the assembled 64x16 LED display: four WS2812B 32x8 panels behind a smoked grey plexiglass front, showing 1234">
  <title>led-display, reference build</title>
  <desc>Face-on view of the assembled display and a section through its layers. Dimensions are the real cut sizes: a 660 by 180 mm sheet carrying a 64 by 16 pixel matrix on a 10 mm pitch.</desc>
  <defs>
    <linearGradient id="plexi" x1="0" y1="0" x2="0.35" y2="1">
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
      .off  { fill: #15171a; }
      .on   { fill: #ffb347; filter: url(#glow); }
      .plexi{ fill: url(#plexi); stroke: #4a5058; stroke-width: 0.8; }
      .alu  { fill: #8a9099; stroke: #6d737b; stroke-width: 0.6; }
      .gap  { fill: none; stroke: #4a5058; stroke-width: 0.7; stroke-dasharray: 5 4; }
      .ledpanel { fill: #14161a; stroke: #2e3238; stroke-width: 0.5; }
      .cut  { fill: #23262b; }
      .lbl  { fill: #8f8e96; font: 11px system-ui, sans-serif; }
      .dimt { fill: #8f8e96; font: 11px system-ui, sans-serif; }
      .diml { stroke: #5a6068; stroke-width: 0.7; }
      .cap  { fill: #ecebe6; font: 600 13px system-ui, sans-serif; }
    </style>
  </defs>

  <rect width="1010" height="330" fill="#0b0b0d"/>

  <!-- front face -->
  <rect x="${OX}" y="${OY}" width="${SHEET_W}" height="${SHEET_H}" rx="4" class="plexi"/>
  <g>
    ${leds}
  </g>
  <rect x="${OX}" y="${OY}" width="${SHEET_W}" height="${SHEET_H}" rx="4" fill="url(#sheen)" pointer-events="none"/>

  <!-- overall width dimension -->
  <line x1="${OX}" y1="${OY + SHEET_H + 14}" x2="${OX + SHEET_W}" y2="${OY + SHEET_H + 14}" class="diml"/>
  <text x="${OX + SHEET_W / 2}" y="${OY + SHEET_H + 28}" class="dimt" text-anchor="middle">660 mm &#183; 64 &#215; 16 pixels &#183; 10 mm pitch</text>

  <text x="${OX + SHEET_W + 14}" y="${OY + 12}" class="cap">Reference build</text>
  <text x="${OX + SHEET_W + 14}" y="${OY + 32}" class="lbl">4 &#215; WS2812B 32&#215;8, 2&#215;2 grid</text>
  <text x="${OX + SHEET_W + 14}" y="${OY + 48}" class="lbl">1024 LEDs, 5 V</text>

  <!-- section through the sandwich -->
  <text x="${OX}" y="${LY - 14}" class="cap">Section</text>
  <g>
    ${stack}
  </g>
  ${panels}
  ${cuts}
  <g>
    ${legend}
  </g>
  <line x1="${OX - 10}" y1="${LY}" x2="${OX - 10}" y2="${LY + stackH}" class="diml"/>
  <text x="${OX - 16}" y="${LY + stackH / 2}" class="dimt" text-anchor="end">36 mm</text>
</svg>
`;

process.stdout.write(svg);
