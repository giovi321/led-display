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

  const badge = (cx, cy, rr, n) =>
    `<circle class="ed-bbg" cx="${cx}" cy="${cy}" r="${rr}"/>` +
    `<text class="ed-btx" x="${cx}" y="${cy}" font-size="${rr}" text-anchor="middle" dy=".35em">${n}</text>`;

  // Clockwise mounting rotation of a panel-local point in LED units
  // (90/270 only when square). Mirrors mapping_xy_to_index().
  function rotPt(p, rot, q) {
    return rot === 90 ? [p.h - q[1], q[0]]
      : rot === 180 ? [p.w - q[0], p.h - q[1]]
      : rot === 270 ? [q[1], p.w - q[0]] : q;
  }

  // Rotation chip: circular arrow + the current mounting angle, so the
  // orientation is readable at a glance and rotating looks tappable. Pure
  // indicator - the whole cell is the tap target.
  function rotChip(cx, cy, h, deg, mini) {
    // mini: degree label only, for drawings too small for the arrow icon.
    const w = h * ((deg >= 100 ? 2.2 : 1.8) + (mini ? 0 : 0.9));
    const x = cx - w / 2, y = cy - h / 2;
    if (mini) {
      return '<g pointer-events="none">' +
        `<rect class="ed-chip" x="${x.toFixed(1)}" y="${y.toFixed(1)}" width="${w.toFixed(1)}"` +
        ` height="${h.toFixed(1)}" rx="${(h / 2).toFixed(1)}"/>` +
        `<text class="ed-chipt" x="${cx.toFixed(1)}" y="${cy.toFixed(1)}" text-anchor="middle"` +
        ` font-size="${(h * 0.66).toFixed(1)}" dy=".35em">${deg}°</text></g>`;
    }
    const ir = h * 0.27, icx = x + h * 0.62, icy = cy;
    const a0 = -0.5, a1 = -1.45;   // arc gap at the top holds the arrowhead
    const pt = (a) => (icx + ir * Math.cos(a)).toFixed(1) + ',' + (icy + ir * Math.sin(a)).toFixed(1);
    const ex = icx + ir * Math.cos(a1), ey = icy + ir * Math.sin(a1);
    const tx = -Math.sin(a1), ty = Math.cos(a1);   // travel direction at the arc end
    const nx = Math.cos(a1), ny = Math.sin(a1);
    const al = ir * 0.95, aw = ir * 0.55;
    const tri = `${(ex + nx * aw).toFixed(1)},${(ey + ny * aw).toFixed(1)} ` +
      `${(ex + tx * al).toFixed(1)},${(ey + ty * al).toFixed(1)} ` +
      `${(ex - nx * aw).toFixed(1)},${(ey - ny * aw).toFixed(1)}`;
    return '<g pointer-events="none">' +
      `<rect class="ed-chip" x="${x.toFixed(1)}" y="${y.toFixed(1)}" width="${w.toFixed(1)}"` +
      ` height="${h.toFixed(1)}" rx="${(h / 2).toFixed(1)}"/>` +
      `<path class="ed-chipa" stroke-width="${(ir * 0.45).toFixed(1)}"` +
      ` d="M${pt(a0)} A${ir.toFixed(1)} ${ir.toFixed(1)} 0 1 1 ${pt(a1)}"/>` +
      `<polygon class="ed-chipf" points="${tri}"/>` +
      `<text class="ed-chipt" x="${(x + h * 1.18).toFixed(1)}" y="${cy.toFixed(1)}"` +
      ` font-size="${(h * 0.62).toFixed(1)}" dy=".35em">${deg}°</text></g>`;
  }

  // On-screen pixels per SVG user unit of the current drawing, so badges,
  // chips and dots keep a readable, non-cartoonish size whatever the
  // viewBox ends up being. Set by buildSvg before the cells render.
  let pxs = 1;

  function cellSvg(r, c) {
    const p = st.panel, cw = p.w * PX, ch = p.h * PX;
    const x = c * cw, y = r * ch, cl = st.cells[r][c];
    const inset = Math.max(1.5, Math.min(cw, ch) * 0.04);
    const box = `x="${x + inset}" y="${y + inset}" width="${cw - 2 * inset}" height="${ch - 2 * inset}"`;
    // Inline style so the identify colour beats the .ed-body class fill.
    const idfy = identify
      ? ` style="fill:${PAL[cl.chain % 8]};fill-opacity:${cl.chain < 8 ? 0.9 : cl.chain < 16 ? 0.5 : 0.3}"`
      : '';
    // First-LED dot on the wiring origin corner, following the mounting
    // rotation - it must match what Identify shows on the wall.
    const o = CORNER[p.origin];
    const din = Math.min(1.6, Math.min(p.w, p.h) * 0.25);
    const dl = rotPt(p, cl.rot, [o[0] ? p.w - din : din, o[1] ? p.h - din : din]);
    const dotR = Math.min(PX * 0.7, 6 / pxs);
    return `<g class="ed-cell" data-r="${r}" data-c="${c}">` +
      `<title>panel ${cl.chain} in the chain, rotated ${cl.rot}° - tap to rotate, drag onto another panel to swap</title>` +
      `<rect class="ed-body" ${box} rx="${(Math.min(cw, ch) * 0.08).toFixed(1)}"${idfy}/>` +
      `<rect ${box} fill="url(#edp)" pointer-events="none"/>` +
      `<circle class="ed-dot" cx="${(x + dl[0] * PX).toFixed(1)}" cy="${(y + dl[1] * PX).toFixed(1)}"` +
      ` r="${dotR.toFixed(1)}" pointer-events="none"/>` + '</g>';
  }

  function chainSvg() {
    const n = st.rows * st.cols;
    if (n < 2) return '';
    const cw = st.panel.w * PX, ch = st.panel.h * PX;
    const pos = new Array(n);
    eachCell((cl, r, c) => { pos[cl.chain] = `${c * cw + cw / 2},${r * ch + ch / 2}`; });
    return `<polyline class="ed-chain" stroke-width="${Math.min(PX * 0.45, 5 / pxs).toFixed(1)}"` +
      ` pointer-events="none" points="${pos.join(' ')}"/>`;
  }

  function buildSvg() {
    const p = st.panel, cw = p.w * PX, ch = p.h * PX;
    const vw = st.cols * cw, vh = st.rows * ch;
    const availW = (ui && ui.wrap && ui.wrap.clientWidth) || 640;
    const availH = window.innerHeight * 0.7;   // matches the max-height cap
    pxs = Math.max(0.05, Math.min(availW / vw, availH / vh));
    let s = `<svg class="ed-svg" viewBox="0 0 ${vw} ${vh}" xmlns="http://www.w3.org/2000/svg">` +
      `<defs><pattern id="edp" width="${PX}" height="${PX}" patternUnits="userSpaceOnUse">` +
      `<circle class="ed-led" cx="${PX / 2}" cy="${PX / 2}" r="${(PX * 0.17).toFixed(1)}"/></pattern></defs>`;
    eachCell((cl, r, c) => { s += cellSvg(r, c); });
    s += chainSvg(); // cable path above bodies, below badges
    eachCell((cl, r, c) => {
      const cx = c * cw + cw / 2, cy = r * ch + ch / 2, m = Math.min(cw, ch);
      const br = Math.min(m * 0.3, 24 / pxs);
      const chipH = Math.min(m * 0.24, 19 / pxs);
      // Wide panels put the chip beside the badge, others below it; cells
      // where the chip would render unreadably small keep just the badge
      // (the title still spells out the rotation).
      const wide = cw >= ch * 1.6;
      const bx = wide ? cx - m * 0.42 : cx;
      const by = wide ? cy : cy - br * 1.1;
      s += `<g pointer-events="none">${badge(bx, by, br.toFixed(1), cl.chain)}</g>`;
      if (chipH * pxs >= 8.5) {
        s += rotChip(wide ? cx + m * 0.55 : cx, wide ? cy : cy + chipH * 1.1,
                     chipH, cl.rot, chipH * pxs < 12);
      }
    });
    return s + '</svg>';
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
      badge(cw / 2, ch / 2, Math.min(m * 0.3, 24 / pxs).toFixed(1),
            st.cells[drag.r][drag.c].chain) + '</g>');
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

  function stopTests() {
    api('/api/display/test', { pattern: 'off' })
      .then((r) => {
        if (!r.ok) throw new Error(`HTTP ${r.status}`);
        identify = false;
        showMsg('tests stopped - normal content restored');
        update();
      })
      .catch((err) => showMsg(`stop failed: ${err.message}`, true));
  }

  // ---- UI ----------------------------------------------------------------

  function showMsg(text, isErr) {
    if (!ui) return;
    ui.msg.textContent = text || '';
    ui.msg.hidden = !text;
    ui.msg.classList.toggle('ed-err', !!isErr);
  }

  // ---- help tooltips ------------------------------------------------------
  // One floating bubble for the circled-? buttons. Hover and keyboard focus
  // show it transiently; click/tap pins it until the next tap or Escape.

  let tipBtn = null, tipPinned = false;

  function hideTip() {
    if (tipBtn) tipBtn.setAttribute('aria-expanded', 'false');
    tipBtn = null;
    tipPinned = false;
    const t = root && root.querySelector('.ed-tip');
    if (t) t.remove();
  }

  function showTip(btn) {
    hideTip();
    tipBtn = btn;
    btn.setAttribute('aria-expanded', 'true');
    const box = root.firstElementChild;   // .ed, position:relative
    const tip = document.createElement('div');
    tip.className = 'ed-tip';
    tip.setAttribute('role', 'tooltip');
    tip.textContent = btn.dataset.help;
    box.appendChild(tip);
    const eb = box.getBoundingClientRect();
    const bb = btn.getBoundingClientRect();
    let left = bb.left - eb.left + bb.width / 2 - tip.offsetWidth / 2;
    left = Math.max(4, Math.min(left, eb.width - tip.offsetWidth - 4));
    tip.style.left = left + 'px';
    tip.style.top = (bb.bottom - eb.top + 6) + 'px';
  }

  function onHelpClick(e) {
    const b = e.target.closest ? e.target.closest('.ed-help') : null;
    if (!b) {
      if (tipBtn) hideTip();
      return;
    }
    e.preventDefault();
    if (tipBtn === b && tipPinned) {
      hideTip();
    } else {
      showTip(b);
      tipPinned = true;
    }
  }

  function onHelpOver(e) {
    const b = e.target.closest ? e.target.closest('.ed-help') : null;
    if (b && tipBtn !== b) showTip(b);
  }

  function onHelpOut(e) {
    const b = e.target.closest ? e.target.closest('.ed-help') : null;
    if (b && tipBtn === b && !tipPinned) hideTip();
  }

  function onHelpFocus(e) {
    if (e.target.classList && e.target.classList.contains('ed-help')) {
      showTip(e.target);
    }
  }

  function onHelpBlur(e) {
    if (e.target === tipBtn) hideTip();
  }

  function onKey(e) {
    if (e.key === 'Escape') hideTip();
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

  const CORNER_NAME = { TL: 'top left', TR: 'top right', BL: 'bottom left', BR: 'bottom right' };

  const originBtn = (k) => {
    const o = CORNER[k];
    return `<button type="button" class="ed-ob" data-k="o${k}" title="first LED in the ${CORNER_NAME[k]} corner">` +
      `<svg width="22" height="22" viewBox="0 0 16 16" aria-hidden="true">` +
      `<rect x="1.5" y="1.5" width="13" height="13" rx="2" class="ed-obr"/>` +
      `<circle cx="${4.2 + o[0] * 7.6}" cy="${4.2 + o[1] * 7.6}" r="2.4" class="ed-dot"/></svg></button>`;
  };

  // Circled question mark; tap/click toggles the explanation, hover and
  // keyboard focus show it too.
  const help = (text) =>
    `<button type="button" class="ed-help" data-help="${text}" aria-expanded="false"` +
    ` aria-label="Explain this setting">` +
    `<svg width="15" height="15" viewBox="0 0 16 16" aria-hidden="true">` +
    `<circle cx="8" cy="8" r="7" class="ed-hc"/>` +
    `<text x="8" y="11.5" text-anchor="middle" class="ed-ht">?</text></svg></button>`;

  const H_ORIGIN = 'The corner where the LED strip enters the panel - its first LED sits there. Shown as the amber dot on every panel in the drawing.';
  const H_AXIS = 'How the strip walks inside one panel: across in rows, or down in columns.';
  const H_SERP = 'The strip reverses direction on every row or column (zigzag) instead of jumping back to the same side. Almost every off-the-shelf matrix is wired this way.';

  // Labeled steppers: each dimension reads as "Rows [-] 2 [+]" with the
  // value boxed between its own buttons, so what each button changes is
  // unambiguous at a glance.
  const stepper = (label, key, valKey) =>
    `<span class="ed-step"><span class="ed-steplab">${label}</span>` +
    `<span class="ed-stepper">${btn(key + '-', '−', `one ${label.toLowerCase().replace(/s$/, '')} fewer`)}` +
    `<b class="ed-val" data-v="${valKey}"></b>` +
    `${btn(key + '+', '+', `one ${label.toLowerCase().replace(/s$/, '')} more`)}</span></span>`;

  const CONTROLS =
    `<div class="ed-row"><span class="ed-lab">Grid</span>` +
    stepper('Rows', 'r', 'rows') +
    stepper('Columns', 'c', 'cols') + `</div>` +
    `<div class="ed-row"><span class="ed-lab">Panel</span>` +
    btn('p8', '8x8') + btn('p16', '16x16') + btn('p32', '8x32 (32x8)') +
    `<label>W <input class="ed-num" data-i="w" type="number" min="1" max="64"></label>` +
    `<label>H <input class="ed-num" data-i="h" type="number" min="1" max="64"></label></div>` +
    `<div class="ed-row"><span class="ed-lab">Wiring</span>` +
    `<span class="ed-wire">` +
    `<span class="ed-fld"><span class="ed-flab">First LED${help(H_ORIGIN)}</span>` +
    `<span class="ed-grp">${['TL', 'TR', 'BL', 'BR'].map(originBtn).join('')}</span></span>` +
    `<span class="ed-fld"><span class="ed-flab">Strip runs${help(H_AXIS)}</span>` +
    `<span class="ed-grp">${btn('arow', 'In rows')}${btn('acol', 'In columns')}</span></span>` +
    `<span class="ed-fld"><span class="ed-flab">Zigzag${help(H_SERP)}</span>` +
    `<label class="ed-check"><input type="checkbox" data-i="serp"> serpentine</label></span>` +
    `</span>` +
    `</div>` +
    `<div class="ed-row">` +
    btn('ident', 'Identify', 'light every physical panel in its chain colour') +
    btn('btest', 'Border test', 'run the border test pattern') +
    btn('stop', 'Stop tests', 'end identify/test mode and restore the normal content') + `</div>` +
    `<div class="ed-banner" hidden><span>unsaved changes</span>${btn('apply', 'Apply')}${btn('revert', 'Revert')}</div>` +
    `<div class="ed-msg" hidden></div>` +
    `<div class="ed-wrap">loading layout...</div>` +
    `<div class="ed-foot"></div>`;

  const CSS =
    '.ed{color:var(--fg,#e6e6e6);position:relative}' +
    '.ed [hidden]{display:none!important}' +
    '.ed label{display:inline-flex;align-items:center;gap:6px;margin:0;' +
    'color:inherit;font-size:inherit}' +
    '.ed-row{display:flex;flex-wrap:wrap;align-items:center;gap:.5em .9em;margin:.6em 0}' +
    '.ed-grp{display:inline-flex;align-items:center;gap:4px}' +
    '.ed-lab{color:var(--mut,#999);min-width:3.4em;font-size:.88em}' +
    '.ed-step{display:inline-flex;align-items:center;gap:8px}' +
    '.ed-steplab{color:var(--mut,#999);font-size:.88em}' +
    '.ed-stepper{display:inline-flex;align-items:stretch;border:1px solid var(--line,#35353d);' +
    'border-radius:8px;overflow:hidden}' +
    '.ed-stepper [data-k]{border:none;border-radius:0;min-width:44px;font-size:19px;' +
    'line-height:1;padding:6px 0}' +
    '.ed-stepper .ed-val{display:inline-flex;align-items:center;justify-content:center;' +
    'min-width:2.4em;background:var(--bg,#0b0b0d);border-left:1px solid var(--line,#35353d);' +
    'border-right:1px solid var(--line,#35353d);font-size:16px;' +
    'font-variant-numeric:tabular-nums}' +
    '.ed-num{width:4em;min-height:38px;background:var(--inp,#232329);color:inherit;' +
    'border:1px solid var(--line,#35353d);border-radius:6px;font:inherit;padding:4px 8px}' +
    '.ed button{font:inherit;cursor:pointer;touch-action:manipulation}' +
    '.ed button:disabled{cursor:not-allowed;opacity:.45}' +
    '.ed [data-k]{background:var(--inp,#232329);color:var(--fg,#e6e6e6);' +
    'border:1px solid var(--line,#35353d);border-radius:6px;min-height:38px;padding:6px 12px}' +
    '.ed [data-k]:not(:disabled):hover{border-color:var(--accent,#4da3ff)}' +
    '.ed-grp>[data-k]{min-width:38px;padding:6px 9px}' +
    '.ed-ob{line-height:0;padding:6px}' +
    '.ed-obr{fill:none;stroke:currentColor;opacity:.55}' +
    '.ed-sel{background:var(--accent,#4da3ff)!important;' +
    'border-color:var(--accent,#4da3ff)!important;color:#000!important;font-weight:600}' +
    '.ed-sel .ed-dot{fill:#000;stroke:none}' +
    '.ed-sel .ed-obr{opacity:.8}' +
    '.ed-wire{display:inline-flex;flex-direction:column;gap:6px}' +
    '.ed-fld{display:flex;align-items:center;gap:8px}' +
    '.ed-flab{color:var(--mut,#999);font-size:.88em;min-width:6.4em;' +
    'display:inline-flex;align-items:center;gap:4px}' +
    '.ed-check{display:inline-flex;align-items:center;gap:7px;min-height:38px;cursor:pointer}' +
    '.ed-check input{width:17px;height:17px}' +
    '.ed .ed-help{background:none;border:none;padding:9px;margin:-5px;line-height:0;' +
    'color:var(--mut,#999);border-radius:50%}' +
    '.ed .ed-help:hover{color:var(--accent,#4da3ff)}' +
    '.ed-hc{fill:none;stroke:currentColor;stroke-width:1.4}' +
    '.ed-ht{fill:currentColor;font-size:10px;font-weight:700}' +
    '.ed-tip{position:absolute;z-index:5;max-width:280px;background:#000;' +
    'border:1px solid var(--accent,#4da3ff);border-radius:6px;padding:8px 10px;' +
    'font-size:.85em;line-height:1.45;box-shadow:0 4px 14px rgba(0,0,0,.5)}' +
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
    '.ed-dot{fill:var(--accent,#4da3ff);stroke:rgba(0,0,0,.55);stroke-width:1}' +
    '.ed-chain{fill:none;stroke:var(--accent,#4da3ff);opacity:.5;stroke-linejoin:round}' +
    '.ed-bbg{fill:rgba(0,0,0,.5)}' +
    '.ed-btx{fill:#fff;font-weight:600;font-family:inherit}' +
    '.ed-chip{fill:rgba(0,0,0,.55);stroke:rgba(255,255,255,.3);stroke-width:1}' +
    '.ed-chipa{fill:none;stroke:#fff;opacity:.92}' +
    '.ed-chipf{fill:#fff;opacity:.92}' +
    '.ed-chipt{fill:#fff;font-weight:600;font-family:inherit}' +
    '.ed-ghost{opacity:.65;pointer-events:none}' +
    '.ed-foot{color:var(--mut,#999);font-size:.85em}';

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
    else if (k === 'stop') stopTests();
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
        banner: q('.ed-banner'), msg: q('.ed-msg'),
        wrap: q('.ed-wrap'), foot: q('.ed-foot'),
      };
      root.querySelectorAll('[data-k]').forEach((b) => { ui.k[b.dataset.k] = b; });
      root.addEventListener('click', onClick);
      root.addEventListener('click', onHelpClick);
      root.addEventListener('mouseover', onHelpOver);
      root.addEventListener('mouseout', onHelpOut);
      root.addEventListener('focusin', onHelpFocus);
      root.addEventListener('focusout', onHelpBlur);
      root.addEventListener('keydown', onKey);
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
      tipBtn = null; tipPinned = false;
      root.removeEventListener('click', onClick);
      root.removeEventListener('click', onHelpClick);
      root.removeEventListener('mouseover', onHelpOver);
      root.removeEventListener('mouseout', onHelpOut);
      root.removeEventListener('focusin', onHelpFocus);
      root.removeEventListener('focusout', onHelpBlur);
      root.removeEventListener('keydown', onKey);
      if (styleEl) styleEl.remove();
      root.innerHTML = '';
      root = null; ui = null; st = null; styleEl = null; drag = null;
      savedJson = ''; busy = false;
    },
  };
})();
