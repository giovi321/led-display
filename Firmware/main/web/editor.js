// editor.js - graphical panel-arrangement editor (DESIGN.md section 2).
//
// Self-contained: window.Editor = { mount(el), leave(), unmount() }. All HTTP goes
// through window.apiFetch (installed by app.js before mount). No libraries;
// injects its own scoped stylesheet (.ed-*) so it does not depend on
// style.css internals. Inherits --bg/--fg/--accent with neutral fallbacks.
(function () {
  'use strict';

  const DEF_LIMITS = { max_rows: 8, max_cols: 16, max_panels: 32, max_pixels: 8192 };
  const PX = 8; // SVG user units per LED
  const CORNER = { TL: [0, 0], TR: [1, 0], BL: [0, 1], BR: [1, 1] };
  // Screen palette mirroring the firmware identify palette (display.c
  // identify_color): vivid equivalents of the {120,0,0}-style values; chains
  // 8..15 / 16+ repeat at reduced opacity like the firmware's half / quarter
  // brightness.
  const PAL = ['#e03030', '#28b428', '#3060e8', '#d4c400', '#c030c0',
               '#00b4b4', '#e87800', '#d8d8d8'];

  let root = null, ui = null, styleEl = null;
  let st = null;        // {panel:{w,h,origin,axis,serpentine}, rows, cols, cells[r][c]={chain,rot}}
  let limits = DEF_LIMITS;
  let savedJson = '';   // last server-acked layout, serialized, for the dirty check
  let identify = false;
  let busy = false;
  let drag = null;      // {r,c,id,sx,sy,moved,ghost,tr,tc}

  // ---- model ----------------------------------------------------------

  const eachCell = (fn) => {
    for (let r = 0; r < st.rows; r++)
      for (let c = 0; c < st.cols; c++) fn(st.cells[r][c], r, c);
  };

  // width/height/limits are server-derived; POST only the inputs.
  function toJson() {
    const cells = [];
    eachCell((cl, row, col) => cells.push({ row, col, chain: cl.chain, rot: cl.rot }));
    const p = st.panel;
    return {
      v: 1,
      panel: { w: p.w, h: p.h, origin: p.origin, axis: p.axis, serpentine: p.serpentine },
      grid: { rows: st.rows, cols: st.cols },
      cells,
    };
  }

  function fromJson(j) {
    limits = Object.assign({}, DEF_LIMITS, j.limits || {});
    const p = j.panel || {}, g = j.grid || {};
    const clamp = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
    st = {
      panel: {
        w: clamp(+p.w || 32, 1, 64), h: clamp(+p.h || 8, 1, 64),
        origin: CORNER[p.origin] ? p.origin : 'TL',
        axis: p.axis === 'row' ? 'row' : 'col',
        serpentine: !!p.serpentine,
      },
      rows: clamp(+g.rows || 1, 1, limits.max_rows),
      cols: clamp(+g.cols || 1, 1, limits.max_cols),
      cells: [],
    };
    for (let r = 0; r < st.rows; r++) st.cells.push(new Array(st.cols).fill(null));
    (j.cells || []).forEach((e) => {
      if (e.row >= 0 && e.row < st.rows && e.col >= 0 && e.col < st.cols)
        st.cells[e.row][e.col] = { chain: +e.chain || 0, rot: +e.rot || 0 };
    });
    // Defensive: fill holes with free chain indices so the editor always
    // holds a permutation even if the payload was partial.
    const used = [];
    st.cells.forEach((row) => row.forEach((cl) => { if (cl) used[cl.chain] = true; }));
    let next = 0;
    st.cells.forEach((row) => row.forEach((cl, i) => {
      if (!cl) {
        while (used[next]) next++;
        used[next] = true;
        row[i] = { chain: next, rot: 0 };
      }
    }));
  }

  const isDirty = () => JSON.stringify(toJson()) !== savedJson;

  function stepReason(nr, nc) {
    if (nr < 1 || nc < 1) return 'minimum 1';
    if (nr > limits.max_rows) return `max ${limits.max_rows} rows`;
    if (nc > limits.max_cols) return `max ${limits.max_cols} cols`;
    if (nr * nc > limits.max_panels) return `max ${limits.max_panels} panels`;
    if (nr * nc * st.panel.w * st.panel.h > limits.max_pixels) return `max ${limits.max_pixels} pixels`;
    return '';
  }

  function panelReason(w, h) {
    if (!(w >= 1 && w <= 64 && h >= 1 && h <= 64)) return 'panel size must be 1..64';
    if (st.rows * st.cols * w * h > limits.max_pixels) return `max ${limits.max_pixels} pixels`;
    return '';
  }

  function setGrid(nr, nc) {
    if (stepReason(nr, nc)) return;
    const old = st.cells, kept = [], grid = [];
    for (let r = 0; r < nr; r++) {
      grid.push([]);
      for (let c = 0; c < nc; c++) {
        const cl = (old[r] && old[r][c]) || null;
        grid[r].push(cl);
        if (cl) kept.push(cl);
      }
    }
    // Shrink: compact surviving chain indices to 0..m-1 preserving their
    // relative order, so the config stays a permutation by construction.
    kept.sort((a, b) => a.chain - b.chain).forEach((cl, i) => { cl.chain = i; });
    // Grow: new cells take the next free indices in reading order.
    let next = kept.length;
    for (let r = 0; r < nr; r++)
      for (let c = 0; c < nc; c++)
        if (!grid[r][c]) grid[r][c] = { chain: next++, rot: 0 };
    st.rows = nr; st.cols = nc; st.cells = grid;
    onChange();
  }

  function setPanelSize(w, h) {
    const why = panelReason(w, h);
    if (why) { showMsg(why, true); update(); return; }
    st.panel.w = w; st.panel.h = h;
    // 90/270 are only legal on square panels; snap to the nearest legal rot.
    if (w !== h) eachCell((cl) => {
      if (cl.rot === 90) cl.rot = 0;
      if (cl.rot === 270) cl.rot = 180;
    });
    onChange();
  }

  function rotate(r, c) {
    const cl = st.cells[r][c];
    cl.rot = (st.panel.w === st.panel.h) ? (cl.rot + 90) % 360 : (cl.rot >= 180 ? 0 : 180);
    onChange();
  }

  // ---- SVG drawing ----------------------------------------------------

  const marker = (id, cls) =>
    `<marker id="${id}" viewBox="0 0 8 8" refX="6.5" refY="4" markerWidth="4.5"` +
    ` markerHeight="4.5" orient="auto"><path class="${cls}" d="M0,0L8,4L0,8Z"/></marker>`;

  const badge = (cx, cy, rr, n) =>
    `<circle class="ed-bbg" cx="${cx}" cy="${cy}" r="${rr}"/>` +
    `<text class="ed-btx" x="${cx}" y="${cy}" font-size="${rr}" text-anchor="middle" dy=".35em">${n}</text>`;

  // Wiring glyph: dot on the first-LED corner plus arrows for the walk
  // direction and the row/column advance, computed in LED units and rotated
  // by the cell's mounting rotation. Geometry mirrors mapping_xy_to_index().
  function glyphSvg(p, rot, x0, y0, px, markId) {
    const w = p.w, h = p.h, o = CORNER[p.origin];
    const sx = o[0] ? -1 : 1, sy = o[1] ? -1 : 1;    // away from the origin corner
    const d1 = p.axis === 'row' ? [sx, 0] : [0, sy]; // strip walk
    const d2 = p.axis === 'row' ? [0, sy] : [sx, 0]; // next row/col
    const padx = Math.min(1.6, w * 0.2), pady = Math.min(1.6, h * 0.2);
    const s = [o[0] * w + sx * padx, o[1] * h + sy * pady];
    const len = Math.max(1.5, (p.axis === 'row' ? w - 2 * padx : h - 2 * pady) * 0.7);
    const step = Math.max(1.2, Math.min(2.5, (p.axis === 'row' ? h : w) * 0.3));
    const at = (q) => { // clockwise mounting rotation (90/270 only when square)
      const t = rot === 90 ? [h - q[1], q[0]]
        : rot === 180 ? [w - q[0], h - q[1]]
        : rot === 270 ? [q[1], w - q[0]] : q;
      return [x0 + t[0] * px, y0 + t[1] * px];
    };
    const pts = (list) => list.map((q) => at(q).map((v) => v.toFixed(1)).join(',')).join(' ');
    const line = (list, mid) =>
      `<polyline class="ed-glyph" ${mid ? `marker-mid="url(#${markId})" ` : ''}` +
      `marker-end="url(#${markId})" points="${pts(list)}"/>`;
    const P1 = [s[0] + d1[0] * len, s[1] + d1[1] * len];
    let out;
    if (p.serpentine) {
      const P2 = [P1[0] + d2[0] * step, P1[1] + d2[1] * step];
      out = line([s, P1, P2, [P2[0] - d1[0] * len, P2[1] - d1[1] * len]], true);
    } else {
      const Q0 = [s[0] + d2[0] * step, s[1] + d2[1] * step];
      out = line([s, P1]) + line([Q0, [Q0[0] + d1[0] * len, Q0[1] + d1[1] * len]]);
    }
    const dp = at(s);
    return out + `<circle class="ed-dot" cx="${dp[0].toFixed(1)}" cy="${dp[1].toFixed(1)}" r="${(px * 0.5).toFixed(1)}"/>`;
  }

  function cellSvg(r, c) {
    const p = st.panel, cw = p.w * PX, ch = p.h * PX;
    const x = c * cw, y = r * ch, cl = st.cells[r][c];
    const inset = Math.max(1.5, Math.min(cw, ch) * 0.04);
    const box = `x="${x + inset}" y="${y + inset}" width="${cw - 2 * inset}" height="${ch - 2 * inset}"`;
    // Inline style so the identify colour beats the .ed-body class fill.
    const idfy = identify
      ? ` style="fill:${PAL[cl.chain % 8]};fill-opacity:${cl.chain < 8 ? 0.9 : cl.chain < 16 ? 0.5 : 0.3}"`
      : '';
    return `<g class="ed-cell" data-r="${r}" data-c="${c}">` +
      `<title>chain ${cl.chain}, rot ${cl.rot}° - drag to swap, tap to rotate</title>` +
      `<rect class="ed-body" ${box} rx="${(Math.min(cw, ch) * 0.08).toFixed(1)}"${idfy}/>` +
      `<rect ${box} fill="url(#edp)" pointer-events="none"/>` +
      glyphSvg(p, cl.rot, x, y, PX, 'edaw') + '</g>';
  }

  function chainSvg() {
    const n = st.rows * st.cols;
    if (n < 2) return '';
    const cw = st.panel.w * PX, ch = st.panel.h * PX;
    const pos = new Array(n);
    eachCell((cl, r, c) => { pos[cl.chain] = `${c * cw + cw / 2},${r * ch + ch / 2}`; });
    return `<polyline class="ed-chain" stroke-width="${(PX * 0.45).toFixed(1)}"` +
      ` marker-mid="url(#edac)" marker-end="url(#edac)" pointer-events="none" points="${pos.join(' ')}"/>`;
  }

  function buildSvg() {
    const p = st.panel, cw = p.w * PX, ch = p.h * PX;
    let s = `<svg class="ed-svg" viewBox="0 0 ${st.cols * cw} ${st.rows * ch}" xmlns="http://www.w3.org/2000/svg">` +
      `<defs><pattern id="edp" width="${PX}" height="${PX}" patternUnits="userSpaceOnUse">` +
      `<circle class="ed-led" cx="${PX / 2}" cy="${PX / 2}" r="${(PX * 0.17).toFixed(1)}"/></pattern>` +
      marker('edaw', 'ed-mw') + marker('edac', 'ed-mc') + '</defs>';
    eachCell((cl, r, c) => { s += cellSvg(r, c); });
    s += chainSvg(); // cable path above bodies, below badges
    eachCell((cl, r, c) => {
      s += `<g pointer-events="none">${badge(c * cw + cw / 2, r * ch + ch / 2, (Math.min(cw, ch) * 0.3).toFixed(1), cl.chain)}</g>`;
    });
    return s + '</svg>';
  }

  // Live preview of the first-panel walk for the current wiring selection.
  function previewSvg() {
    const p = st.panel, px = 5, w = p.w * px, h = p.h * px;
    const sc = Math.min(110 / w, 64 / h);
    return `<svg width="${Math.round(w * sc)}" height="${Math.round(h * sc)}" viewBox="0 0 ${w} ${h}">` +
      `<defs>${marker('edawp', 'ed-mw')}</defs>` +
      `<rect class="ed-body" x="1" y="1" width="${w - 2}" height="${h - 2}" rx="${(Math.min(w, h) * 0.08).toFixed(1)}"/>` +
      glyphSvg(p, 0, 0, 0, px, 'edawp') + '</svg>';
  }

  // ---- interactions (pointer events, touch included) -------------------

  function toSvgPoint(e) {
    const svg = ui.wrap.querySelector('svg');
    const m = svg && svg.getScreenCTM && svg.getScreenCTM();
    return m ? new DOMPoint(e.clientX, e.clientY).matrixTransform(m.inverse()) : null;
  }

  function makeGhost() {
    const svg = ui.wrap.querySelector('svg');
    const cw = st.panel.w * PX, ch = st.panel.h * PX, m = Math.min(cw, ch);
    svg.insertAdjacentHTML('beforeend',
      `<g class="ed-ghost"><rect class="ed-body" width="${cw}" height="${ch}" rx="${(m * 0.08).toFixed(1)}"/>` +
      badge(cw / 2, ch / 2, (m * 0.3).toFixed(1), st.cells[drag.r][drag.c].chain) + '</g>');
    drag.ghost = svg.lastElementChild;
  }

  function setTarget(r, c) {
    if (r < 0 || r >= st.rows || c < 0 || c >= st.cols || (r === drag.r && c === drag.c)) { r = -1; c = -1; }
    if (r === drag.tr && c === drag.tc) return;
    const svg = ui.wrap.querySelector('svg');
    const prev = svg.querySelector('.ed-over');
    if (prev) prev.classList.remove('ed-over');
    drag.tr = r; drag.tc = c;
    if (r >= 0) {
      const g = svg.querySelector(`.ed-cell[data-r="${r}"][data-c="${c}"]`);
      if (g) g.classList.add('ed-over');
    }
  }

  function onDown(e) {
    if (busy || drag || !st) return;
    const g = e.target.closest ? e.target.closest('.ed-cell') : null;
    if (!g) return;
    e.preventDefault();
    drag = { r: +g.dataset.r, c: +g.dataset.c, id: e.pointerId,
             sx: e.clientX, sy: e.clientY, moved: false, ghost: null, tr: -1, tc: -1 };
    try { ui.wrap.setPointerCapture(e.pointerId); } catch (_) { /* best effort */ }
  }

  function onMove(e) {
    if (!drag || e.pointerId !== drag.id) return;
    // 6 px hysteresis so a shaky tap still counts as a rotate click.
    if (!drag.moved && Math.hypot(e.clientX - drag.sx, e.clientY - drag.sy) < 6) return;
    drag.moved = true;
    const pt = toSvgPoint(e);
    if (!pt) return;
    if (!drag.ghost || !drag.ghost.isConnected) makeGhost();
    const cw = st.panel.w * PX, ch = st.panel.h * PX;
    drag.ghost.setAttribute('transform', `translate(${(pt.x - cw / 2).toFixed(1)},${(pt.y - ch / 2).toFixed(1)})`);
    setTarget(Math.floor(pt.y / ch), Math.floor(pt.x / cw));
  }

  function onUp(e) {
    if (!drag || e.pointerId !== drag.id) return;
    const d = drag;
    drag = null;
    if (d.ghost) d.ghost.remove();
    if (!d.moved) {
      rotate(d.r, d.c);
    } else if (d.tr >= 0) {
      // Swap the two cells wholesale (chain + rot travel together), which
      // keeps the chain assignment a permutation by construction.
      const t = st.cells[d.tr][d.tc];
      st.cells[d.tr][d.tc] = st.cells[d.r][d.c];
      st.cells[d.r][d.c] = t;
      onChange();
    } else {
      update(); // dropped outside: just clear the highlight
    }
  }

  function onCancel(e) {
    if (!drag || e.pointerId !== drag.id) return;
    if (drag.ghost) drag.ghost.remove();
    drag = null;
    update();
  }

  // ---- API --------------------------------------------------------------

  const api = (path, body) => window.apiFetch(path, body === undefined ? undefined : {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });

  function load() {
    busy = true;
    api('/api/layout')
      .then((r) => { if (!r.ok) throw new Error(`HTTP ${r.status}`); return r.json(); })
      .then((j) => {
        fromJson(j);
        savedJson = JSON.stringify(toJson());
        showMsg('');
      })
      .catch((err) => showMsg(`failed to load layout: ${err.message}`, true))
      .then(() => { busy = false; update(); });
  }

  function apply() {
    if (busy) return;
    busy = true;
    update();
    const body = toJson();
    api('/api/layout', body)
      .then((r) => {
        if (r.ok) {
          savedJson = JSON.stringify(body);
          showMsg('applied - check the border test on the panel');
          return undefined;
        }
        return r.text().then((t) => {
          let m = t;
          try { const j = JSON.parse(t); m = j.error || j.message || t; } catch (_) { /* raw text */ }
          showMsg(m || `HTTP ${r.status}`, true);
        });
      })
      .catch((err) => showMsg(`apply failed: ${err.message}`, true))
      .then(() => { busy = false; update(); });
  }

  function toggleIdentify() {
    const on = !identify;
    api('/api/display/identify', { on })
      .then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        identify = on;
        update();
      })
      .catch((err) => showMsg(`identify failed: ${err.message}`, true));
  }

  function borderTest() {
    api('/api/display/test', { pattern: 'border' })
      .then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        identify = false; // test patterns end identify mode on the firmware side
        showMsg('border test running');
        update();
      })
      .catch((err) => showMsg(`test failed: ${err.message}`, true));
  }

  // ---- UI ----------------------------------------------------------------

  function showMsg(text, isErr) {
    if (!ui) return;
    ui.msg.textContent = text || '';
    ui.msg.hidden = !text;
    ui.msg.classList.toggle('ed-err', !!isErr);
  }

  function onChange() { showMsg(''); update(); }

  function setStep(btn, why) { btn.disabled = !!why; btn.title = why || ''; }

  function update() {
    if (!st || !ui) return;
    const p = st.panel, K = ui.k;
    ui.rowsVal.textContent = st.rows;
    ui.colsVal.textContent = st.cols;
    setStep(K['r-'], stepReason(st.rows - 1, st.cols));
    setStep(K['r+'], stepReason(st.rows + 1, st.cols));
    setStep(K['c-'], stepReason(st.rows, st.cols - 1));
    setStep(K['c+'], stepReason(st.rows, st.cols + 1));
    setStep(K.p8, panelReason(8, 8));
    setStep(K.p16, panelReason(16, 16));
    setStep(K.p32, panelReason(32, 8));
    if (document.activeElement !== ui.w) ui.w.value = p.w;
    if (document.activeElement !== ui.h) ui.h.value = p.h;
    ['TL', 'TR', 'BL', 'BR'].forEach((k) => K['o' + k].classList.toggle('ed-sel', p.origin === k));
    K.arow.classList.toggle('ed-sel', p.axis === 'row');
    K.acol.classList.toggle('ed-sel', p.axis === 'col');
    ui.serp.checked = p.serpentine;
    ui.prev.innerHTML = previewSvg();
    K.ident.classList.toggle('ed-sel', identify);
    K.ident.setAttribute('aria-pressed', identify ? 'true' : 'false');
    ui.banner.hidden = !isDirty();
    K.apply.disabled = busy;
    K.revert.disabled = busy;
    ui.wrap.innerHTML = buildSvg();
    const n = st.rows * st.cols, pixels = n * p.w * p.h;
    ui.foot.textContent =
      `${st.cols * p.w}x${st.rows * p.h} px, ${n} panels, refresh ~${(33 * 1024 / pixels).toFixed(1)} Hz`;
  }

  const btn = (k, label, title) =>
    `<button type="button" data-k="${k}"${title ? ` title="${title}"` : ''}>${label}</button>`;

  const originBtn = (k) => {
    const o = CORNER[k];
    return `<button type="button" class="ed-ob" data-k="o${k}" title="first LED at ${k}">` +
      `<svg width="18" height="18" viewBox="0 0 16 16"><rect x="1" y="1" width="14" height="14" rx="2" class="ed-obr"/>` +
      `<circle cx="${3 + o[0] * 10}" cy="${3 + o[1] * 10}" r="2.6" class="ed-dot"/></svg></button>`;
  };

  const CONTROLS =
    `<div class="ed-row"><span class="ed-lab">Grid</span>` +
    `<span class="ed-grp">rows ${btn('r-', '−')}<b class="ed-val" data-v="rows"></b>${btn('r+', '+')}</span>` +
    `<span class="ed-grp">cols ${btn('c-', '−')}<b class="ed-val" data-v="cols"></b>${btn('c+', '+')}</span></div>` +
    `<div class="ed-row"><span class="ed-lab">Panel</span>` +
    btn('p8', '8x8') + btn('p16', '16x16') + btn('p32', '8x32 (32x8)') +
    `<label>W <input class="ed-num" data-i="w" type="number" min="1" max="64"></label>` +
    `<label>H <input class="ed-num" data-i="h" type="number" min="1" max="64"></label></div>` +
    `<div class="ed-row"><span class="ed-lab">Wiring</span>` +
    `<span class="ed-grp">${['TL', 'TR', 'BL', 'BR'].map(originBtn).join('')}</span>` +
    `<span class="ed-grp">${btn('arow', 'rows', 'strip walks rows first')}${btn('acol', 'columns', 'strip walks columns first')}</span>` +
    `<label><input type="checkbox" data-i="serp"> serpentine</label>` +
    `<span class="ed-prev" title="first-panel walk preview"></span></div>` +
    `<div class="ed-row">` +
    btn('ident', 'Identify', 'light every physical panel in its chain colour') +
    btn('btest', 'Border test', 'run the border test pattern') + `</div>` +
    `<div class="ed-banner" hidden><span>unsaved changes</span>${btn('apply', 'Apply')}${btn('revert', 'Revert')}</div>` +
    `<div class="ed-msg" hidden></div>` +
    `<div class="ed-wrap">loading layout...</div>` +
    `<div class="ed-foot"></div>`;

  const CSS =
    '.ed{color:var(--fg,#e6e6e6)}' +
    '.ed-row{display:flex;flex-wrap:wrap;align-items:center;gap:.6em;margin:.45em 0}' +
    '.ed-grp{display:inline-flex;align-items:center;gap:.25em}' +
    '.ed-lab{opacity:.6;min-width:3.2em}' +
    '.ed-val{min-width:1.4em;text-align:center}' +
    '.ed-num{width:3.6em}' +
    '.ed button{cursor:pointer}' +
    '.ed button:disabled{cursor:not-allowed;opacity:.45}' +
    '.ed-ob{padding:2px;line-height:0}' +
    '.ed-obr{fill:none;stroke:currentColor;opacity:.6}' +
    '.ed-sel{outline:2px solid var(--accent,#4da3ff);outline-offset:1px}' +
    '.ed-banner{display:flex;align-items:center;gap:.7em;margin:.5em 0;padding:.4em .7em;' +
    'border:1px solid var(--accent,#4da3ff);border-radius:6px;background:rgba(77,163,255,.08)}' +
    '.ed-msg{margin:.4em 0;padding:.3em .6em;border-radius:4px;background:rgba(128,128,128,.12)}' +
    '.ed-msg.ed-err{color:#e05555}' +
    '.ed-wrap{margin:.6em 0}' +
    '.ed-svg{display:block;width:100%;height:auto;max-height:70vh;touch-action:none;' +
    'user-select:none;-webkit-user-select:none}' +
    '.ed-cell{cursor:grab}' +
    '.ed-body{fill:rgba(128,128,128,.13);stroke:rgba(128,128,128,.55);stroke-width:1.5}' +
    '.ed-over .ed-body{stroke:var(--accent,#4da3ff);stroke-width:4}' +
    '.ed-led{fill:var(--fg,#888);opacity:.13}' +
    '.ed-glyph{fill:none;stroke:var(--fg,#bbb);stroke-width:2;opacity:.75}' +
    '.ed-mw{fill:var(--fg,#bbb)}' +
    '.ed-dot{fill:var(--accent,#4da3ff)}' +
    '.ed-chain{fill:none;stroke:var(--accent,#4da3ff);opacity:.5;stroke-linejoin:round}' +
    '.ed-mc{fill:var(--accent,#4da3ff)}' +
    '.ed-bbg{fill:rgba(0,0,0,.5)}' +
    '.ed-btx{fill:#fff;font-weight:600;font-family:inherit}' +
    '.ed-ghost{opacity:.65;pointer-events:none}' +
    '.ed-prev{line-height:0;opacity:.9}' +
    '.ed-prev svg{display:block}' +
    '.ed-foot{opacity:.6;font-size:.85em}';

  function onClick(e) {
    const b = e.target.closest ? e.target.closest('[data-k]') : null;
    if (!b || b.disabled || !st) return;
    const k = b.dataset.k;
    if (k === 'r-') setGrid(st.rows - 1, st.cols);
    else if (k === 'r+') setGrid(st.rows + 1, st.cols);
    else if (k === 'c-') setGrid(st.rows, st.cols - 1);
    else if (k === 'c+') setGrid(st.rows, st.cols + 1);
    else if (k === 'p8') setPanelSize(8, 8);
    else if (k === 'p16') setPanelSize(16, 16);
    else if (k === 'p32') setPanelSize(32, 8);
    else if (k[0] === 'o' && CORNER[k.slice(1)]) { st.panel.origin = k.slice(1); onChange(); }
    else if (k === 'arow') { st.panel.axis = 'row'; onChange(); }
    else if (k === 'acol') { st.panel.axis = 'col'; onChange(); }
    else if (k === 'ident') toggleIdentify();
    else if (k === 'btest') borderTest();
    else if (k === 'apply') apply();
    else if (k === 'revert') load();
  }

  window.Editor = {
    // Called by app.js when the user navigates away from the Hardware tab:
    // identify mode must not keep the physical panels frozen in chain
    // colours after the editor goes out of sight. Keeps all edit state.
    leave() {
      if (identify) toggleIdentify();
    },
    mount(containerEl) {
      if (root) window.Editor.unmount();
      root = containerEl;
      styleEl = document.createElement('style');
      styleEl.textContent = CSS;
      document.head.appendChild(styleEl);
      root.innerHTML = `<div class="ed">${CONTROLS}</div>`;
      const q = (sel) => root.querySelector(sel);
      ui = {
        k: {},
        rowsVal: q('[data-v="rows"]'), colsVal: q('[data-v="cols"]'),
        w: q('[data-i="w"]'), h: q('[data-i="h"]'), serp: q('[data-i="serp"]'),
        prev: q('.ed-prev'), banner: q('.ed-banner'), msg: q('.ed-msg'),
        wrap: q('.ed-wrap'), foot: q('.ed-foot'),
      };
      root.querySelectorAll('[data-k]').forEach((b) => { ui.k[b.dataset.k] = b; });
      root.addEventListener('click', onClick);
      ui.serp.addEventListener('change', () => {
        if (st) { st.panel.serpentine = ui.serp.checked; onChange(); }
      });
      ui.w.addEventListener('change', () => {
        if (st) setPanelSize(parseInt(ui.w.value, 10) || 0, st.panel.h);
      });
      ui.h.addEventListener('change', () => {
        if (st) setPanelSize(st.panel.w, parseInt(ui.h.value, 10) || 0);
      });
      ui.wrap.addEventListener('pointerdown', onDown);
      ui.wrap.addEventListener('pointermove', onMove);
      ui.wrap.addEventListener('pointerup', onUp);
      ui.wrap.addEventListener('pointercancel', onCancel);
      load();
    },
    unmount() {
      if (!root) return;
      if (identify) {
        // Best effort: leave the physical display in normal mode.
        try { api('/api/display/identify', { on: false }); } catch (_) { /* ignore */ }
      }
      identify = false;
      root.removeEventListener('click', onClick);
      if (styleEl) styleEl.remove();
      root.innerHTML = '';
      root = null; ui = null; st = null; styleEl = null; drag = null;
      savedJson = ''; busy = false;
    },
  };
})();
