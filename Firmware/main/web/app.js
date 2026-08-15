// led-display v2 web UI. Vanilla JS, one page; the tab sections live in
// index.html and this file wires them to the JSON API (DESIGN.md section 5).
// editor.js loads before this file and provides window.Editor, which the
// Hardware tab mounts on first open. Served gzipped from /app.js.

'use strict';

// ---- API fetch wrapper -----------------------------------------------------
// Defined before anything else so editor.js (and every handler below) can use
// it. Adds X-Api-Key from localStorage to every request; on a 401 prompts
// once for the key, stores it, and retries the request.
window.apiFetch = async function (path, opts) {
  opts = opts || {};
  const send = () => {
    const headers = Object.assign({}, opts.headers || {});
    const key = localStorage.getItem('apikey');
    if (key && !headers['X-Api-Key']) headers['X-Api-Key'] = key;
    if (typeof opts.body === 'string' && !headers['Content-Type']) {
      headers['Content-Type'] = 'application/json';
    }
    return fetch(path, Object.assign({}, opts, { headers: headers }));
  };
  let res = await send();
  if (res.status === 401) {
    const k = prompt('This device requires an API key:');
    if (k !== null && k.trim() !== '') {
      localStorage.setItem('apikey', k.trim());
      res = await send();
    }
  }
  return res;
};

// ---- tiny helpers ----------------------------------------------------------

const $ = s => document.querySelector(s);
const $$ = s => Array.from(document.querySelectorAll(s));
const sleep = ms => new Promise(r => setTimeout(r, ms));

function esc(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
  }[c]));
}
function kv(k, v) {
  return '<div class="kv"><b>' + esc(k) + '</b><span>' + esc(v) + '</span></div>';
}

let toastTimer = 0;
function toast(msg, isErr) {
  const t = $('#toast');
  t.textContent = msg;
  t.classList.toggle('err', !!isErr);
  t.classList.remove('hidden');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => t.classList.add('hidden'), 2600);
}

// Fetch JSON, throwing a readable Error on any non-2xx response so callers
// can just await and let act() turn failures into an error toast.
async function jfetch(path, opts) {
  const res = await window.apiFetch(path, opts);
  let j = null;
  try { j = await res.json(); } catch (e) { /* non-JSON body */ }
  if (!res.ok) {
    throw new Error((j && j.error) ? j.error : ('HTTP ' + res.status));
  }
  return j;
}
const jget = p => jfetch(p);
const jpost = (p, body) => jfetch(p, {
  method: 'POST',
  body: body === undefined ? undefined : JSON.stringify(body),
});

// Run an async action with the triggering button disabled while it is in
// flight; failures become an error toast instead of an unhandled rejection.
async function act(btn, fn) {
  if (btn.disabled) return;
  btn.disabled = true;
  try {
    await fn();
  } catch (e) {
    toast('Error: ' + (e && e.message ? e.message : e), true);
  } finally {
    btn.disabled = false;
  }
}

function hex2rgb(h) {
  const v = parseInt(h.slice(1), 16);
  return { r: (v >> 16) & 255, g: (v >> 8) & 255, b: v & 255 };
}
function rgb2hex(c) {
  return '#' + [c.r, c.g, c.b].map(v => (v & 255).toString(16).padStart(2, '0')).join('');
}
function fmtAge(secs) {
  if (secs < 90) return secs + ' s';
  if (secs < 5400) return Math.round(secs / 60) + ' min';
  if (secs < 172800) return Math.round(secs / 3600) + ' h';
  return Math.round(secs / 86400) + ' d';
}
function fmtUptime(secs) {
  const d = Math.floor(secs / 86400), h = Math.floor(secs / 3600) % 24,
        m = Math.floor(secs / 60) % 60, s = Math.floor(secs % 60);
  if (d) return d + 'd ' + h + 'h ' + m + 'm';
  if (h) return h + 'h ' + m + 'm';
  return m + 'm ' + s + 's';
}

// ---- circled-? help tooltips -----------------------------------------------
// One floating bubble for every <button class="hlp" data-help="..."> on the
// page. Hover and keyboard focus show it transiently; click/tap pins it until
// the next tap or Escape. Same interaction as the arrangement editor's tips,
// which keep their own copy because editor.js is self-contained.

let tipBtn = null, tipPinned = false;

function hideTip() {
  if (tipBtn) tipBtn.setAttribute('aria-expanded', 'false');
  tipBtn = null;
  tipPinned = false;
  const t = $('#tip');
  if (t) t.remove();
}

function showTip(btn) {
  hideTip();
  tipBtn = btn;
  btn.setAttribute('aria-expanded', 'true');
  const tip = document.createElement('div');
  tip.id = 'tip';
  tip.className = 'tip';
  tip.setAttribute('role', 'tooltip');
  tip.textContent = btn.dataset.help;
  document.body.appendChild(tip);
  // position:fixed, so viewport coordinates are all the maths needed.
  const b = btn.getBoundingClientRect();
  let left = b.left + b.width / 2 - tip.offsetWidth / 2;
  left = Math.max(8, Math.min(left, window.innerWidth - tip.offsetWidth - 8));
  tip.style.left = left + 'px';
  tip.style.top = (b.bottom + 6) + 'px';
}

const hlpOf = e => (e.target.closest ? e.target.closest('.hlp') : null);

document.addEventListener('click', e => {
  const b = hlpOf(e);
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
});
document.addEventListener('mouseover', e => {
  const b = hlpOf(e);
  if (b && tipBtn !== b) showTip(b);
});
document.addEventListener('mouseout', e => {
  const b = hlpOf(e);
  if (b && tipBtn === b && !tipPinned) hideTip();
});
document.addEventListener('focusin', e => {
  if (e.target.classList && e.target.classList.contains('hlp')) showTip(e.target);
});
document.addEventListener('focusout', e => { if (e.target === tipBtn) hideTip(); });
document.addEventListener('keydown', e => { if (e.key === 'Escape') hideTip(); });

// ---- shared state ----------------------------------------------------------
// NOTE: not named "status" - a top-level var of that name would collide with
// window.status (a string-coercing browser property).

let lastStatus = null;    // last /api/status payload
let colors = [];          // saved colour presets [{name,r,g,b}]
let editorMounted = false;
let savedNets = [];       // saved WiFi networks [{ssid,connected}]
let savedMax = 5;

// ---- tabs + advanced mode --------------------------------------------------

const ADV_TABS = ['hardware', 'network', 'integrations', 'system'];

function setAdvanced(on) {
  localStorage.setItem('advanced', on ? '1' : '0');
  $('#advtoggle').checked = on;
  $$('#tabs .adv').forEach(b => b.classList.toggle('hidden', !on));
  const cur = $('#tabs button.active');
  if (!on && cur && ADV_TABS.indexOf(cur.dataset.tab) >= 0) tab('display');
}

function tab(name) {
  // Leaving the Hardware tab must switch identify mode off, or the physical
  // panels stay frozen in the chain colours with no visible way back.
  const prev = $('#tabs button.active');
  if (prev && prev.dataset.tab === 'hardware' && name !== 'hardware') {
    if (window.Editor && typeof window.Editor.leave === 'function') {
      window.Editor.leave();
    }
    lsPollStop();   // stop the light-sensor live poll with the tab
  }
  $$('#tabs button').forEach(b => b.classList.toggle('active', b.dataset.tab === name));
  $$('main > section').forEach(sec => sec.classList.toggle('hidden', sec.id !== 'tab-' + name));
  if (name === 'schedule') refreshSchedule();
  if (name === 'hardware') { openHardware(); lsPollStart(); }
  if (name === 'network') refreshNetwork();
  if (name === 'integrations') refreshIntegrations();
  if (name === 'system') refreshSystem();
}

$$('#tabs button').forEach(b => { b.onclick = () => tab(b.dataset.tab); });
$('#advtoggle').onchange = e => setAdvanced(e.target.checked);

// ---- Display tab -----------------------------------------------------------

// Fonts baked into this firmware image (fonts.h); the size select only
// offers combinations whose pixel height fits the active display.
const FONTS = [
  { name: '3x5', h: 5 },
  { name: '5x7', h: 7 },
  { name: '7x12', h: 12 },
];

// font:scale the device last reported, even when it no longer fits the layout:
// the firmware keeps a stored font that is too tall and renders a smaller one
// instead, so the select has to be able to hold it.
let wantSize = null;

function buildSizeOptions(dispH) {
  const sel = $('#size');
  const prev = sel.value || wantSize;
  const opts = [];
  FONTS.forEach(f => {
    for (let s = 1; s <= 8 && f.h * s <= dispH; s++) {
      opts.push({ v: f.name + ':' + s, px: f.h * s, f: f.name, s: s });
    }
  });
  opts.sort((a, b) => a.px - b.px || a.f.localeCompare(b.f));
  sel.innerHTML = '';
  opts.forEach(o => {
    const el = document.createElement('option');
    el.value = o.v;
    el.textContent = o.px + ' px · ' + o.f + (o.s > 1 ? ' ×' + o.s : '');
    sel.appendChild(el);
  });
  // Without an option for the stored size the select would end up with no
  // selection at all, and Apply would post an empty font and be rejected
  // wholesale (text, colour, scrolling and brightness with it).
  if (wantSize && !opts.some(o => o.v === wantSize)) {
    const p = wantSize.split(':');
    const el = document.createElement('option');
    el.value = wantSize;
    el.textContent = p[0] + (+p[1] > 1 ? ' ×' + p[1] : '')
      + ' - taller than this display';
    sel.appendChild(el);
  }
  if (prev && Array.from(sel.options).some(o => o.value === prev)) sel.value = prev;
}
buildSizeOptions(16);   // sane default until /api/status reports the layout

function setScrollUi() {
  $('#speed').disabled = $('#scrollmode').value === 'off';
}
$('#scrollmode').onchange = setScrollUi;
setScrollUi();

$('#brightness').oninput = e => { $('#brval').textContent = e.target.value; };
$('#speed').oninput = e => { $('#spval').textContent = e.target.value; };

// The light sensor and the slider are the same brightness: while the sensor is
// driving it, the slider reads out the level it picked and stops being an input.
// The stored level is untouched, so switching the sensor off restores it.
// Mirrors the firmware's arbiter rather than the stored flag alone: the sensor
// only owns the brightness once it has produced a level, and never in setup-AP
// mode, where the panel carries the only copy of the AP name and address.
function sensorDriven(s) {
  const ls = s && s.light_sensor;
  return !!(ls && ls.enabled && typeof ls.level === 'number' && ls.level >= 0
            && s.wifi && s.wifi.mode !== 'ap');
}
function setBrightnessAuto(on) {
  $('#brightness').disabled = !!on;
  $('#br-auto').classList.toggle('hidden', !on);
}

// The schedule's off-window level is a brightness too: it lives under the same
// cap as the main slider.
function capSchedDim(cap) {
  const el = $('#sched-dim');
  el.max = Math.max(1, cap);
  if (+el.value > el.max) {
    el.value = el.max;
    $('#sched-dimval').textContent = el.value;
  }
}

// ---- content source (static text vs integration value) ---------------------

function setSourceUi(src) {
  $$('#srcseg button').forEach(b => b.classList.toggle('active', b.dataset.src === src));
  $('#static-card').classList.toggle('hidden', src !== 'static');
  $('#src-note').classList.toggle('hidden', src !== 'integration');
}

$$('#srcseg button').forEach(b => {
  b.onclick = () => act(b, async () => {
    await jpost('/api/display/source', { mode: b.dataset.src });
    setSourceUi(b.dataset.src);
    toast(b.dataset.src === 'static'
      ? 'Showing the static text'
      : 'Showing the integration value');
    try { applyStatus(await jget('/api/status')); } catch (e) { /* poll catches up */ }
  });
});

// The static text has its own card and button; the appearance card posts
// everything EXCEPT the text so it works in both source modes without
// clobbering the stored static text.
$('#apply-text').onclick = () => act($('#apply-text'), async () => {
  await jpost('/api/display/text', { text: $('#text').value });
  toast('Text updated');
  try { applyStatus(await jget('/api/status')); } catch (e) { /* poll catches up */ }
});

$('#apply').onclick = () => act($('#apply'), async () => {
  const raw = $('#size').value;
  if (!raw) { toast('Pick a text size first', true); return; }
  const size = raw.split(':');
  const body = {
    color: hex2rgb($('#color').value),
    scroll_mode: $('#scrollmode').value,
    speed: +$('#speed').value,
    font: size[0],
    scale: +size[1] || 1,
    spacing: $('#spacing').value,
  };
  await jpost('/api/display/text', body);
  // Don't post the slider back while the sensor owns the brightness: that would
  // overwrite the user's stored level with whatever the room happens to be.
  const auto = sensorDriven(lastStatus);
  if (!auto) {
    await jpost('/api/display/brightness', { brightness: +$('#brightness').value });
  }
  toast(auto ? 'Applied - brightness follows the light sensor' : 'Applied');
  try { applyStatus(await jget('/api/status')); } catch (e) { /* poll catches up */ }
});

$('#clear').onclick = () => act($('#clear'), async () => {
  await jpost('/api/display/clear');
  toast('Display cleared');
  try { applyStatus(await jget('/api/status')); } catch (e) { /* poll catches up */ }
});

// ---- saved colour chips ----------------------------------------------------
// GET may return a bare array or {colors:[...]}; POST always sends the whole
// list as {colors:[...]} (DESIGN.md section 4: max 12 entries).

async function loadColors() {
  try {
    const j = await jget('/api/colors');
    colors = Array.isArray(j) ? j : (j && Array.isArray(j.colors) ? j.colors : []);
  } catch (e) {
    colors = [];
  }
  renderChips();
}
function saveColors() {
  return jpost('/api/colors', { colors: colors });
}

function renderChips() {
  const box = $('#chips');
  box.innerHTML = '';
  colors.forEach((c, i) => {
    const chip = document.createElement('button');
    chip.type = 'button';
    chip.className = 'chip';
    chip.style.background = rgb2hex(c);
    // Pick a readable label colour for the chip's background.
    const lum = 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
    chip.style.color = lum > 140 ? '#000' : '#fff';
    chip.textContent = c.name || rgb2hex(c);
    chip.title = 'Tap to apply, long-press or × to remove';
    const x = document.createElement('span');
    x.className = 'x';
    x.textContent = '×';
    x.setAttribute('aria-hidden', 'true');
    chip.appendChild(x);

    let pressTimer = 0, longPressed = false;
    chip.addEventListener('pointerdown', () => {
      longPressed = false;
      pressTimer = setTimeout(() => { longPressed = true; removeChip(i); }, 650);
    });
    const cancel = () => clearTimeout(pressTimer);
    chip.addEventListener('pointerup', cancel);
    chip.addEventListener('pointerleave', cancel);
    chip.addEventListener('contextmenu', ev => ev.preventDefault());
    chip.addEventListener('click', ev => {
      if (longPressed) { ev.preventDefault(); return; }
      if (ev.target === x) { removeChip(i); return; }
      applyChip(c);
    });
    box.appendChild(chip);
  });
  if (colors.length < 12) {
    const add = document.createElement('button');
    add.type = 'button';
    add.className = 'chip add';
    add.textContent = '+';
    add.title = 'Save the current picker colour';
    add.setAttribute('aria-label', 'Save the current picker colour');
    add.onclick = addChip;
    box.appendChild(add);
  }
}

async function applyChip(c) {
  $('#color').value = rgb2hex(c);
  try {
    // Partial POST: the handler keeps text/scroll/speed when absent.
    await jpost('/api/display/text', { color: { r: c.r, g: c.g, b: c.b } });
    toast('Colour applied');
  } catch (e) {
    toast('Error: ' + e.message, true);
  }
}

async function removeChip(i) {
  const gone = colors.splice(i, 1)[0];
  renderChips();
  try {
    await saveColors();
    toast('Removed ' + ((gone && gone.name) || 'colour'));
  } catch (e) {
    toast('Error: ' + e.message, true);
    loadColors();  // resync with the device
  }
}

async function addChip() {
  if (colors.length >= 12) { toast('Limit of 12 saved colours', true); return; }
  const hex = $('#color').value;
  const name = prompt('Name for this colour:', hex);
  if (name === null) return;
  const c = hex2rgb(hex);
  colors.push({ name: (name.trim() || hex).slice(0, 16), r: c.r, g: c.g, b: c.b });
  renderChips();
  try {
    await saveColors();
    toast('Saved ' + hex);
  } catch (e) {
    toast('Error: ' + e.message, true);
    loadColors();
  }
}

// ---- Schedule tab ----------------------------------------------------------

// Off-window action: the dim slider only means something in "dim" mode.
function schedOffAction() {
  const on = $('#sched-offact input[type=radio]:checked');
  return on ? on.value : 'off';
}
function setSchedOffUi() {
  $('#sched-dim').disabled = schedOffAction() !== 'dim';
}
$$('#sched-offact input[type=radio]').forEach(r => { r.onchange = setSchedOffUi; });
$('#sched-dim').oninput = e => { $('#sched-dimval').textContent = e.target.value; };
setSchedOffUi();

// Timezone: the zone list comes from the browser's own tzdata, so the device
// never ships a name list and the picker is always current. The device holds
// only the IANA-name-to-rule table it needs to apply the choice.
const TZ_FALLBACK = [
  'UTC', 'Europe/London', 'Europe/Zurich', 'Europe/Berlin', 'Europe/Madrid',
  'Europe/Rome', 'Europe/Athens', 'America/New_York', 'America/Chicago',
  'America/Denver', 'America/Los_Angeles', 'Asia/Tokyo', 'Asia/Kolkata',
  'Asia/Singapore', 'Australia/Sydney',
];

function browserZone() {
  try {
    return Intl.DateTimeFormat().resolvedOptions().timeZone || '';
  } catch (e) { return ''; }
}

function buildTzOptions(current) {
  const sel = $('#sched-tz');
  let zones = [];
  try {
    if (typeof Intl.supportedValuesOf === 'function') {
      zones = Intl.supportedValuesOf('timeZone');
    }
  } catch (e) { /* older browser */ }
  if (!zones.length) zones = TZ_FALLBACK.slice();
  // A zone the device already holds but this browser doesn't know about
  // must still be selectable, or saving would silently change it.
  if (current && zones.indexOf(current) < 0) zones = zones.concat([current]);

  const here = browserZone();
  sel.innerHTML = '';
  const fixed = document.createElement('option');
  fixed.value = '';
  fixed.textContent = 'Fixed offset from this browser';
  sel.appendChild(fixed);
  zones.forEach(z => {
    const o = document.createElement('option');
    o.value = z;
    o.textContent = z.replace(/_/g, ' ') + (z === here ? '  (this browser)' : '');
    sel.appendChild(o);
  });
  sel.value = current || '';
}

function setSchedTzUi(known) {
  const v = $('#sched-tz').value;
  const note = $('#sched-tznote');
  if (!v) {
    note.textContent = 'The offset your browser is on when you press Save is '
      + 'copied to the device. It will not follow daylight-saving changes by '
      + 'itself - re-save after a clock change, or pick a zone instead.';
    note.classList.remove('warn');
  } else if (known === false) {
    note.textContent = 'This device does not recognise "' + v + '" and will '
      + 'run on UTC. Pick another zone.';
    note.classList.add('warn');
  } else {
    note.textContent = 'Daylight-saving changes for ' + v.replace(/_/g, ' ')
      + ' are applied by the device itself.';
    note.classList.remove('warn');
  }
}
$('#sched-tz').onchange = () => setSchedTzUi();

async function refreshSchedule() {
  try {
    const s = await jget('/api/schedule');
    $('#sched-en').checked = !!s.enabled;
    $('#sched-on').value = String(s.on_hour).padStart(2, '0') + ':' + String(s.on_min).padStart(2, '0');
    $('#sched-off').value = String(s.off_hour).padStart(2, '0') + ':' + String(s.off_min).padStart(2, '0');
    $$('#sched-days input').forEach(cb => {
      cb.checked = !!(s.days & (1 << +cb.dataset.day));
    });
    // Pre-2.2 firmware reports neither field: that build always turned off.
    const offAct = s.off_action === 'dim' ? 'dim' : 'off';
    $$('#sched-offact input[type=radio]').forEach(r => { r.checked = r.value === offAct; });
    const cap = (lastStatus && lastStatus.system.brightness_cap) || 255;
    const dim = $('#sched-dim');
    dim.max = Math.max(1, cap);
    dim.value = Math.min(s.off_brightness || 8, cap);
    $('#sched-dimval').textContent = dim.value;
    setSchedOffUi();
    buildTzOptions(s.tz || '');
    setSchedTzUi(s.tz ? !!s.tz_known : undefined);
  } catch (e) { /* first load can race the device; keep defaults */ }
}

$('#sched-save').onclick = () => act($('#sched-save'), async () => {
  const on = $('#sched-on').value.split(':');
  const off = $('#sched-off').value.split(':');
  let days = 0;
  $$('#sched-days input').forEach(cb => { if (cb.checked) days |= (1 << +cb.dataset.day); });
  const tz = $('#sched-tz').value;
  await jpost('/api/schedule', {
    enabled: $('#sched-en').checked,
    on_hour: +on[0], on_min: +on[1],
    off_hour: +off[0], off_min: +off[1],
    days: days,
    off_action: schedOffAction(),
    off_brightness: +$('#sched-dim').value || 1,
    // IANA zone name; "" falls back to the fixed offset below.
    tz: tz,
    // Minutes east of UTC, taken from the browser so the device's schedule
    // matches the wall clock the user is looking at.
    utc_offset: -new Date().getTimezoneOffset(),
  });
  toast('Schedule saved');
});

// ---- Hardware tab ----------------------------------------------------------

function openHardware() {
  if (!editorMounted) {
    const root = document.getElementById('editor-root');
    if (window.Editor && typeof window.Editor.mount === 'function') {
      window.Editor.mount(root);
      editorMounted = true;
    } else {
      root.innerHTML = '<p class="note">Arrangement editor failed to load. Reload the page.</p>';
    }
  }
  if (lastStatus) $('#brcap').value = lastStatus.system.brightness_cap || 255;
  refreshLightSensor();
}

$$('#tab-hardware [data-test]').forEach(b => {
  b.onclick = () => act(b, async () => {
    await jpost('/api/display/test', { pattern: b.dataset.test });
    toast(b.dataset.test === 'off'
      ? 'Tests stopped - normal content restored'
      : 'Test: ' + b.dataset.test);
  });
});

// ---- light sensor (Hardware tab) -------------------------------------------
// Calibration needs live numbers, so the card polls the sensor endpoint while
// the tab is open. The device samples whenever a pin is configured, enabled or
// not, which is what makes "cover it, press Use current" work before handing
// the brightness over.

let lsTimer = 0;
let lsRawMax = 4095;
let lsLive = null;       // last polled {sampling, raw, level}

function lsGpioOptions(list) {
  const sel = $('#ls-gpio');
  const prev = sel.value;
  const pins = (list && list.length) ? list : [1, 2, 3, 4, 5, 6, 7, 8, 9, 10];
  sel.innerHTML = '';
  const none = document.createElement('option');
  none.value = '0';
  none.textContent = 'Not connected';
  sel.appendChild(none);
  pins.forEach(g => {
    const o = document.createElement('option');
    o.value = String(g);
    o.textContent = 'GPIO' + g;
    sel.appendChild(o);
  });
  if (prev) sel.value = prev;
}

function setLsUi() {
  const hasPin = +$('#ls-gpio').value > 0;
  ['#ls-en', '#ls-dark', '#ls-bright', '#ls-min', '#ls-max', '#ls-gamma',
   '#ls-usedark', '#ls-usebright'].forEach(s => {
    $(s).disabled = !hasPin;
  });
  if (!hasPin) $('#ls-en').checked = false;
  $('#ls-minval').textContent = $('#ls-min').value;
  $('#ls-maxval').textContent = $('#ls-max').value;
  $('#ls-gammaval').textContent = (+$('#ls-gamma').value / 10).toFixed(1);
  $('#ls-hint').textContent = hasPin
    ? 'Cover the sensor and press Use current for the dark reading, then light '
      + 'it and do the same for the bright one.'
    : 'Pick the pin your sensor is wired to, then save. The device samples it '
      + 'once a second and the live reading appears here.';
}

function renderLsLive(j) {
  lsLive = j || null;
  const box = $('#ls-live');
  if (!j || !j.sampling) {
    box.innerHTML = kv('Live reading', 'no sensor pin saved');
    return;
  }
  const level = j.level >= 0 ? String(j.level)
              : (j.enabled ? 'waiting for a reading' : 'sensor off');
  box.innerHTML =
    kv('Live reading', j.raw < 0 ? 'waiting...' : j.raw + ' of ' + lsRawMax) +
    kv('Brightness now', level);
}

async function refreshLightSensor() {
  try {
    const j = await jget('/api/display/light_sensor');
    if (j.raw_max) lsRawMax = j.raw_max;
    lsGpioOptions(j.valid_gpios);
    $('#ls-gpio').value = String(j.gpio || 0);
    $('#ls-en').checked = !!j.enabled;
    $('#ls-dark').value = j.dark_raw;
    $('#ls-bright').value = j.bright_raw;
    $('#ls-min').value = j.min_brightness;
    $('#ls-max').value = j.max_brightness;
    $('#ls-gamma').value = j.gamma_x10 || 10;
    setLsUi();
    renderLsLive(j);
  } catch (e) {
    lsGpioOptions(null);
    setLsUi();
    renderLsLive(null);
  }
}

function lsPollStop() {
  if (lsTimer) { clearInterval(lsTimer); lsTimer = 0; }
}
function lsPollStart() {
  lsPollStop();
  lsTimer = setInterval(async () => {
    try {
      renderLsLive(await jget('/api/display/light_sensor'));
    } catch (e) { /* transient - the next tick tries again */ }
  }, 2000);
}

function useLiveReading(sel) {
  if (!lsLive || !lsLive.sampling || lsLive.raw < 0) {
    toast('No reading yet - save a sensor pin first', true);
    return;
  }
  $(sel).value = lsLive.raw;
  toast('Captured ' + lsLive.raw);
}

$('#ls-gpio').onchange = setLsUi;
$('#ls-min').oninput = setLsUi;
$('#ls-max').oninput = setLsUi;
$('#ls-gamma').oninput = setLsUi;
$('#ls-usedark').onclick = () => useLiveReading('#ls-dark');
$('#ls-usebright').onclick = () => useLiveReading('#ls-bright');

$('#ls-save').onclick = () => act($('#ls-save'), async () => {
  const gpio = +$('#ls-gpio').value;
  const dark = Math.round(+$('#ls-dark').value);
  const bright = Math.round(+$('#ls-bright').value);
  if ($('#ls-en').checked && !gpio) { toast('Pick a sensor pin first', true); return; }
  if (dark === bright) { toast('Dark and bright readings must differ', true); return; }
  await jpost('/api/display/light_sensor', {
    enabled: $('#ls-en').checked,
    gpio: gpio,
    dark_raw: dark,
    bright_raw: bright,
    min_brightness: +$('#ls-min').value,
    max_brightness: +$('#ls-max').value,
    gamma_x10: +$('#ls-gamma').value,
  });
  toast($('#ls-en').checked ? 'Light sensor on' : 'Light sensor saved');
  await refreshLightSensor();
  // The Display tab's slider follows the sensor, so re-pull the status now.
  try { applyStatus(await jget('/api/status')); } catch (e) { /* poll will catch up */ }
});

$('#brcap-save').onclick = () => act($('#brcap-save'), async () => {
  let v = Math.round(+$('#brcap').value);
  if (!(v >= 1 && v <= 255)) { toast('Cap must be 1-255', true); return; }
  await jpost('/api/display/brightness_cap', { cap: v });
  // Reflect the new ceiling on the Display and Schedule tabs immediately.
  const br = $('#brightness');
  br.max = v;
  if (+br.value > v) { br.value = v; $('#brval').textContent = v; }
  capSchedDim(v);
  toast('Brightness cap set to ' + v);
});

// ---- Network tab -----------------------------------------------------------

// Saved networks: SSIDs plus which one the device is on. The API never
// returns passwords, so the list is display-only apart from Forget.
async function refreshSavedNets() {
  try {
    const j = await jget('/api/wifi/networks');
    savedNets = (j && j.networks) || [];
    if (j && j.max) savedMax = j.max;
  } catch (e) {
    savedNets = [];   // pre-2.2 firmware has no saved-network endpoint
  }
  renderSavedNets();
}

function renderSavedNets() {
  const box = $('#savednets');
  box.innerHTML = '';
  savedNets.forEach(n => {
    const row = document.createElement('div');
    row.className = 'snet';
    const left = document.createElement('span');
    left.className = 'ssid';
    if (n.connected) {
      const dot = document.createElement('span');
      dot.className = 'dot ok';
      dot.setAttribute('aria-hidden', 'true');
      left.appendChild(dot);
    }
    const name = document.createElement('span');
    name.textContent = n.ssid;
    left.appendChild(name);
    if (n.connected) {
      const tag = document.createElement('span');
      tag.className = 'tag';
      tag.textContent = 'connected';
      left.appendChild(tag);
    }
    const del = document.createElement('button');
    del.type = 'button';
    del.className = 'sec';
    del.textContent = 'Forget';
    del.setAttribute('aria-label', 'Forget ' + n.ssid);
    del.onclick = () => act(del, async () => {
      if (!confirm('Forget "' + n.ssid + '"?')) return;
      await jpost('/api/wifi/networks/delete', { ssid: n.ssid });
      toast('Forgot ' + n.ssid);
      await refreshSavedNets();
    });
    row.appendChild(left);
    row.appendChild(del);
    box.appendChild(row);
  });

  const note = $('#savednote');
  if (!savedNets.length) {
    note.textContent = 'None saved - the device starts its own setup network '
      + 'until you add one.';
  } else if (savedNets.length >= savedMax) {
    note.textContent = savedNets.length + ' of ' + savedMax
      + ' saved - forget one before adding another.';
  } else {
    note.textContent = savedNets.length + ' of ' + savedMax + ' saved.';
  }
}

async function refreshNetwork() {
  try {
    const s = await jget('/api/status');
    lastStatus = s;
    $('#wifistate').innerHTML =
      kv('Mode', s.wifi.mode) +
      kv('SSID', s.wifi.ssid || '-') +
      kv('IP', s.wifi.ip || '-') +
      kv('RSSI', (s.wifi.rssi || '-') + ' dBm');
    renderWgStatus(s.wireguard);
  } catch (e) {
    toast('Error: ' + e.message, true);
    return;
  }
  refreshSavedNets();
  if (lastStatus.wireguard && lastStatus.wireguard.compiled_in) {
    try {
      const c = await jget('/api/wireguard');
      $('#wg-en').checked = !!c.enabled;
      // The API may withhold the private key; the field stays blank and the
      // save handler only sends it when the user typed a new one.
      if (c.private_key) $('#wg-priv').value = c.private_key;
      $('#wg-pub').value = c.peer_public_key || '';
      $('#wg-host').value = c.endpoint_host || '';
      $('#wg-port').value = c.endpoint_port || '';
      $('#wg-addr').value = c.address || '';
      $('#wg-allowed').value = c.allowed_ips || '';
      $('#wg-keep').value = (c.keepalive_s === undefined ? 25 : c.keepalive_s);
      setWgFullTunnelUi();
    } catch (e) { /* endpoint not up yet; leave the form editable */ }
  }
}

function renderWgStatus(wg) {
  const missing = !wg || !wg.compiled_in;
  $('#wg-missing').classList.toggle('hidden', !missing);
  $('#wg-body').classList.toggle('hidden', missing);
  if (missing) return;
  let line;
  if (!wg.enabled) {
    line = 'Tunnel disabled.';
  } else if (wg.held) {
    line = 'Tunnel suspended after repeated reboots - check the configuration and save again to retry.';
  } else if (!wg.up) {
    line = 'Tunnel enabled - not connected.';
  } else {
    const hs = wg.last_handshake > 0
      ? fmtAge(Math.max(0, Math.floor(Date.now() / 1000) - wg.last_handshake)) + ' ago'
      : 'never';
    line = 'Tunnel up - last handshake ' + hs + '.';
  }
  $('#wg-status').textContent = line;
}

$('#scan').onclick = () => act($('#scan'), async () => {
  $('#nets').innerHTML = '<p class="note">Scanning...</p>';
  const r = await jget('/api/wifi/scan');
  const nets = (r && r.networks) || [];
  const box = $('#nets');
  if (r && r.error) {
    // The radio is busy - usually a reconnect pass working through the
    // saved networks.
    box.innerHTML = '<p class="note warn">Scan unavailable right now ('
      + esc(r.error) + '). Try again in a moment.</p>';
    return;
  }
  box.innerHTML = nets.length ? '' : '<p class="note">No networks found.</p>';
  nets.forEach(n => {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'net';
    const ssid = document.createElement('span');
    ssid.className = 'ssid';
    ssid.textContent = n.ssid;
    // Already-saved networks need no password retyped, so say so.
    if (n.saved) {
      const tag = document.createElement('span');
      tag.className = 'tag';
      tag.textContent = ' saved';
      ssid.appendChild(tag);
    }
    const rssi = document.createElement('span');
    rssi.className = 'rssi';
    rssi.textContent = n.rssi + ' dBm';
    btn.appendChild(ssid);
    btn.appendChild(rssi);
    btn.onclick = () => { $('#wssid').value = n.ssid; $('#wpass').focus(); };
    box.appendChild(btn);
  });
});

$('#wadd').onclick = () => act($('#wadd'), async () => {
  const ssid = $('#wssid').value.trim();
  if (!ssid) { toast('Need an SSID', true); return; }
  await jpost('/api/wifi/networks', { ssid: ssid, password: $('#wpass').value });
  $('#wpass').value = '';
  toast('Saved ' + ssid);
  await refreshSavedNets();
});

$('#wconnect').onclick = () => act($('#wconnect'), async () => {
  const ssid = $('#wssid').value.trim();
  const password = $('#wpass').value;
  if (!ssid) { toast('Need an SSID', true); return; }
  await jpost('/api/wifi/connect', { ssid: ssid, password: password });
  toast('Saved - the device is rebooting onto ' + ssid);
});

$('#wreset').onclick = () => act($('#wreset'), async () => {
  const n = savedNets.length;
  if (!confirm('Forget ' + (n ? 'all ' + n + ' saved network' + (n === 1 ? '' : 's')
                              : 'all saved networks')
               + ' and reboot into setup mode?')) return;
  await jpost('/api/wifi/reset');
  toast('Forgetting all networks...');
});

// A full-tunnel route takes the local reply path with it; warn before saving
// rather than after losing the connection.
function setWgFullTunnelUi() {
  const v = $('#wg-allowed').value;
  const full = /(^|[,\s])0\.0\.0\.0\/0([,\s]|$)/.test(v);
  $('#wg-fulltunnel').classList.toggle('hidden', !full);
}
$('#wg-allowed').oninput = setWgFullTunnelUi;

$('#wg-reveal').onclick = () => {
  const f = $('#wg-priv');
  const show = f.type === 'password';
  f.type = show ? 'text' : 'password';
  $('#wg-reveal').textContent = show ? 'Hide' : 'Show';
  $('#wg-reveal').setAttribute('aria-label', (show ? 'Hide' : 'Show') + ' private key');
};

$('#wg-save').onclick = () => act($('#wg-save'), async () => {
  const cfg = {
    enabled: $('#wg-en').checked,
    peer_public_key: $('#wg-pub').value.trim(),
    endpoint_host: $('#wg-host').value.trim(),
    endpoint_port: Math.round(+$('#wg-port').value) || 0,
    address: $('#wg-addr').value.trim(),
    allowed_ips: $('#wg-allowed').value.trim(),
    keepalive_s: Math.round(+$('#wg-keep').value) || 0,
  };
  const pk = $('#wg-priv').value.trim();
  if (pk) cfg.private_key = pk;  // blank = keep the stored key
  await jpost('/api/wireguard', cfg);
  toast('WireGuard config saved');
  refreshNetwork();
});

// ---- Integrations tab ------------------------------------------------------

async function refreshIntegrations() {
  try {
    const j = await jget('/api/integrations');
    // The device name is a System-tab setting now (one name, also used as the
    // hostname and the setup-AP SSID); this endpoint still reports it.
    $('#whurl').value = j.webhook_url || '';
    // The wire format is an array of event names, not a bitmask.
    const evts = Array.isArray(j.webhook_events) ? j.webhook_events : [];
    $$('#whevents input').forEach(cb => { cb.checked = evts.includes(cb.dataset.evt); });
    // The key itself may be withheld by the API; only report whether one is set.
    const keySet = !!(j.api_key || j.api_key_set);
    $('#apikey-state').textContent = keySet
      ? 'A key is currently set.'
      : 'No key set - all requests are allowed.';
  } catch (e) {
    toast('Error: ' + e.message, true);
  }
  try {
    const v = await jget('/api/value/config');
    $('#v-prefix').value = v.prefix || '';
    $('#v-suffix').value = v.suffix || '';
    $('#v-dec').value = (v.decimals === undefined ? -1 : v.decimals);
    $('#v-thou').value = v.thousands || '';
    $('#v-stalehrs').value = v.stale_hours || 0;
    const actName = (typeof v.stale_action === 'number')
      ? (['none', 'dim', 'marker', 'text'][v.stale_action] || 'none')
      : (v.stale_action || 'none');
    $('#v-staleact').value = actName;
    $('#v-staletext').value = v.stale_text || '';
    $('#v-default').value = v.default_text || '';
  } catch (e) { /* endpoint not up yet; leave the form editable */ }
  await refreshMqtt();
  updateCurl();
}

async function refreshMqtt() {
  try {
    const m = await jget('/api/mqtt');
    const missing = !(m.status && m.status.compiled_in);
    $('#mqtt-missing').classList.toggle('hidden', !missing);
    $('#mqtt-body').classList.toggle('hidden', missing);
    if (missing) return;
    $('#mq-en').checked = !!m.enabled;
    $('#mq-uri').value = m.uri || '';
    $('#mq-user').value = m.username || '';
    $('#mq-base').value = m.base_topic || '';
    $('#mq-ha').checked = !!m.ha_discovery;
    $('#mq-haprefix').value = m.ha_prefix || '';
    $('#mqtt-status').textContent = !m.enabled
      ? 'Disabled.'
      : (m.status.connected ? 'Connected to the broker.'
                            : 'Enabled - not connected yet.');
  } catch (e) { /* pre-MQTT firmware: leave the card as it is */ }
}

$('#mq-save').onclick = () => act($('#mq-save'), async () => {
  const cfg = {
    enabled: $('#mq-en').checked,
    uri: $('#mq-uri').value.trim(),
    username: $('#mq-user').value.trim(),
    base_topic: $('#mq-base').value.trim(),
    ha_discovery: $('#mq-ha').checked,
    ha_prefix: $('#mq-haprefix').value.trim(),
  };
  const pw = $('#mq-pass').value;
  if (pw) cfg.password = pw;  // blank = keep the stored one
  if (cfg.enabled && !cfg.uri) { toast('Enter the broker URI first', true); return; }
  await jpost('/api/mqtt', cfg);
  $('#mq-pass').value = '';
  toast('MQTT config saved');
  refreshMqtt();
});

function updateCurl() {
  const host = (lastStatus && lastStatus.wifi.ip) || '<device-ip>';
  $('#curl-ex').textContent =
    "curl -X POST http://" + host + "/api/value -H 'X-Api-Key: KEY' -d '{\"value\":42}'";
}

$('#apikey-set').onclick = () => act($('#apikey-set'), async () => {
  const v = $('#apikey').value.trim();
  if (!v) { toast('Enter a key first', true); return; }
  await jpost('/api/integrations', { api_key: v });
  // Adopt the new key locally so this UI keeps working immediately.
  localStorage.setItem('apikey', v);
  $('#apikey').value = '';
  toast('API key set');
  refreshIntegrations();
});

$('#apikey-clear').onclick = () => act($('#apikey-clear'), async () => {
  if (!confirm('Clear the API key? All requests will be allowed again.')) return;
  await jpost('/api/integrations', { api_key: '' });
  localStorage.removeItem('apikey');
  toast('API key cleared');
  refreshIntegrations();
});

$('#wh-save').onclick = () => act($('#wh-save'), async () => {
  const evts = [];
  $$('#whevents input').forEach(cb => { if (cb.checked) evts.push(cb.dataset.evt); });
  await jpost('/api/integrations', {
    webhook_url: $('#whurl').value.trim(),
    webhook_events: evts,
  });
  toast('Webhook config saved');
});

$('#vcfg-save').onclick = () => act($('#vcfg-save'), async () => {
  await jpost('/api/value/config', {
    prefix: $('#v-prefix').value,
    suffix: $('#v-suffix').value,
    decimals: Math.round(+$('#v-dec').value),
    thousands: $('#v-thou').value,
    stale_hours: Math.max(0, Math.round(+$('#v-stalehrs').value) || 0),
    stale_action: $('#v-staleact').value,
    stale_text: $('#v-staletext').value,
    default_text: $('#v-default').value,
  });
  toast('Value config saved');
});

$('#v-send').onclick = () => act($('#v-send'), async () => {
  const raw = $('#v-test').value.trim();
  if (!raw) { toast('Enter a test value first', true); return; }
  const num = Number(raw);
  const j = await jpost('/api/value', { value: isNaN(num) ? raw : num });
  $('#v-echo').textContent = 'Displayed: ' + ((j && j.displayed) || '(no echo in response)');
  toast('Value sent');
});

$('#curl-copy').onclick = () => act($('#curl-copy'), async () => {
  const txt = $('#curl-ex').textContent;
  try {
    await navigator.clipboard.writeText(txt);
  } catch (e) {
    // Plain-HTTP pages get no clipboard API; fall back to execCommand.
    const ta = document.createElement('textarea');
    ta.value = txt;
    document.body.appendChild(ta);
    ta.select();
    document.execCommand('copy');
    ta.remove();
  }
  toast('Copied');
});

// ---- System tab ------------------------------------------------------------

// ---- device name -----------------------------------------------------------
// One name, three uses. Mirrors the firmware's rule (devname.h) so the field
// can say what is wrong before a round trip; the device validates again.

const DEVNAME_RE = /^[A-Za-z0-9]([A-Za-z0-9-]{0,22}[A-Za-z0-9])?$/;

function devNameError(v) {
  if (v === '') return '';    // empty = back to the defaults
  if (v.length > 24) return 'Up to 24 characters.';
  if (!DEVNAME_RE.test(v)) {
    return 'Letters, digits and hyphens only, starting and ending with a '
      + 'letter or digit.';
  }
  return '';
}

function renderDevNameUses(name, apSsid) {
  $('#devname-uses').innerHTML =
    kv('Hostname', name) +
    kv('Setup network', apSsid || name) +
    kv('Webhook "device"', name);
}

// The header carries the device name once one is set, and the browser tab with
// it - several panels on one desk are otherwise indistinguishable.
function setDeviceTitle(name, isSet) {
  const label = (isSet && name) ? name : 'LED Display';
  $('#hdrname').textContent = label;
  if (document.title !== label) document.title = label;
}

$('#devname').oninput = () => {
  const err = devNameError($('#devname').value.trim());
  const el = $('#devname-err');
  el.textContent = err;
  el.classList.toggle('hidden', !err);
};

$('#devname-save').onclick = () => act($('#devname-save'), async () => {
  const v = $('#devname').value.trim();
  const err = devNameError(v);
  if (err) { toast(err, true); return; }
  const j = await jpost('/api/system/device_name', { name: v });
  if (j && j.device_name) {
    setDeviceTitle(j.device_name, !!v);
    renderDevNameUses(j.device_name, j.ap_ssid);
  }
  toast(v ? 'Device name: ' + v : 'Device name cleared');
  refreshSystem();
});

async function refreshSystem() {
  try {
    const s = await jget('/api/status');
    lastStatus = s;
    $('#sys').innerHTML =
      kv('Firmware', 'v' + s.system.fw) +
      kv('OTA slot', s.ota.slot + (s.ota.version ? ' (' + s.ota.version + ')' : '')) +
      kv('Free heap', Math.round(s.system.heap / 1024) + ' KB') +
      kv('Uptime', fmtUptime(s.system.uptime)) +
      kv('IP', s.wifi.ip || '-') +
      kv('RSSI', (s.wifi.rssi || '-') + ' dBm');
    if (s.system.boot_screen) $('#bootscr').value = s.system.boot_screen;
    // Empty when no name is stored: the placeholder then shows the default
    // rather than pre-filling a name the user never chose.
    $('#devname').value = s.system.device_name_stored || '';
    $('#devname-err').classList.add('hidden');
    renderDevNameUses(s.system.device_name || 'led-display',
                      s.wifi.ap_ssid || '');
    $('#foot').textContent = 'led-display v' + s.system.fw;
  } catch (e) {
    toast('Error: ' + e.message, true);
  }
}

$('#bootscr-save').onclick = () => act($('#bootscr-save'), async () => {
  await jpost('/api/system/boot_screen', { mode: $('#bootscr').value });
  toast('Boot screen: ' + $('#bootscr').value);
});

$('#reboot').onclick = () => act($('#reboot'), async () => {
  if (!confirm('Reboot the device now?')) return;
  await jpost('/api/system/reboot');
  toast('Rebooting...');
});

$('#freset').onclick = () => act($('#freset'), async () => {
  if (!confirm('Factory reset: wipe all display settings? WiFi credentials are kept.')) return;
  const j = await jpost('/api/system/factory_reset');
  toast((j && j.note) || 'Factory reset - rebooting');
});

// ---- OTA -------------------------------------------------------------------

function setBar(frac) {
  const bar = $('#ota-bar');
  bar.classList.remove('hidden');
  bar.classList.toggle('indet', frac < 0);
  $('#ota-fill').style.transform = 'scaleX(' + (frac < 0 ? 1 : Math.min(1, frac)) + ')';
}

// Raw-body upload via XHR (fetch has no upload progress). Mirrors apiFetch's
// key handling: X-Api-Key from localStorage, one prompt-and-retry on 401.
function otaUploadXhr(file, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota/upload');
    const key = localStorage.getItem('apikey');
    if (key) xhr.setRequestHeader('X-Api-Key', key);
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.upload.onprogress = e => {
      if (e.lengthComputable) onProgress(e.loaded / e.total);
    };
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) { resolve(); return; }
      const err = new Error('upload failed (HTTP ' + xhr.status + ')');
      err.status = xhr.status;
      reject(err);
    };
    xhr.onerror = () => reject(new Error('upload failed (connection lost)'));
    xhr.send(file);
  });
}

async function otaUpload(file, onProgress) {
  try {
    await otaUploadXhr(file, onProgress);
  } catch (e) {
    if (e.status === 401) {
      const k = prompt('This device requires an API key:');
      if (k !== null && k.trim() !== '') {
        localStorage.setItem('apikey', k.trim());
        await otaUploadXhr(file, onProgress);
        return;
      }
    }
    throw e;
  }
}

// Poll /api/ota/status until the device either finishes without rebooting
// (e.g. a failed download: last_result explains why) or drops the connection
// because the reboot started.
async function pollOta(msgEl) {
  let lastResult = '';
  let idleTicks = 0;
  for (let n = 0; n < 600; n++) {
    await sleep(1000);
    let st;
    try {
      st = await jget('/api/ota/status');
    } catch (e) {
      return { rebooting: true, lastResult: lastResult };
    }
    if (st.in_progress) {
      idleTicks = 0;
      const pct = st.progress_pct;
      msgEl.textContent = 'Updating... ' + (pct >= 0 ? pct + '%' : '(size unknown)');
      setBar(pct >= 0 ? pct / 100 : -1);
    } else {
      lastResult = st.last_result || lastResult;
      if (lastResult) msgEl.textContent = lastResult;
      // Give the device a few seconds to start its reboot; if it stays up,
      // the update ended on this image (usually a failure - see last_result).
      if (++idleTicks >= 5) return { rebooting: false, lastResult: lastResult };
    }
  }
  return { rebooting: false, lastResult: lastResult };
}

// The device closes connections while it reboots; poll /api/status with a
// visible countdown until it answers again (or we give up after ~3 min).
async function waitForReboot(msgEl) {
  const start = Date.now();
  for (let n = 0; n < 90; n++) {
    await sleep(2000);
    msgEl.textContent = 'Rebooting... waiting for the device ('
      + Math.round((Date.now() - start) / 1000) + ' s)';
    try {
      const res = await fetch('/api/status', { cache: 'no-store' });
      if (res.ok) return await res.json();
    } catch (e) { /* still down - keep waiting */ }
  }
  return null;
}

function finishOta(s) {
  if (s) {
    lastStatus = s;
    $('#ota-msg').textContent = 'Back online - firmware v' + s.system.fw;
    $('#foot').textContent = 'led-display v' + s.system.fw;
    toast('Update complete - running v' + s.system.fw);
    refreshSystem();
  } else {
    $('#ota-msg').textContent =
      'The device did not come back yet - reload this page manually.';
    toast('Device not reachable yet', true);
  }
}

$('#ota-upload').onclick = () => act($('#ota-upload'), async () => {
  const f = $('#ota-file').files[0];
  if (!f) { toast('Choose a .bin file first', true); return; }
  if (!confirm('Install "' + f.name + '"? The device reboots when done.')) return;
  setBar(0);
  $('#ota-msg').textContent = 'Uploading...';
  await otaUpload(f, frac => {
    setBar(frac);
    $('#ota-msg').textContent = 'Uploading... ' + Math.round(frac * 100) + '%';
  });
  $('#ota-msg').textContent = 'Upload complete - waiting for the device';
  const r = await pollOta($('#ota-msg'));
  if (r.rebooting || !r.lastResult) {
    finishOta(await waitForReboot($('#ota-msg')));
  } else {
    toast(r.lastResult, /fail|err|abort|invalid/i.test(r.lastResult));
  }
});

$('#ota-pull').onclick = () => act($('#ota-pull'), async () => {
  const url = $('#ota-url').value.trim();
  if (!url) { toast('Enter a firmware URL first', true); return; }
  if (!confirm('Download and install firmware from:\n' + url + '\nThe device reboots when done.')) return;
  await jpost('/api/ota/url', { url: url });
  setBar(-1);
  $('#ota-msg').textContent = 'Update started...';
  const r = await pollOta($('#ota-msg'));
  if (r.rebooting) {
    finishOta(await waitForReboot($('#ota-msg')));
  } else {
    toast(r.lastResult || 'Update finished',
          /fail|err|abort|invalid/i.test(r.lastResult || ''));
  }
});

// GitHub release check: the device queries its configured repo's latest
// release and offers the led_display.bin asset through the same URL-pull
// flow as a manual update.
$('#ota-check').onclick = () => act($('#ota-check'), async () => {
  $('#ota-check-msg').textContent = 'Checking GitHub...';
  $('#ota-install').classList.add('hidden');
  const r = await jget('/api/ota/check');
  if (!r.ok) {
    $('#ota-check-msg').textContent = r.error || 'Check failed';
    return;
  }
  if (r.update_available) {
    $('#ota-check-msg').textContent =
      'Update available: ' + r.current + ' -> ' + r.latest;
    const btn = $('#ota-install');
    btn.classList.remove('hidden');
    btn.onclick = () => act(btn, async () => {
      if (!confirm('Install v' + r.latest + ' from GitHub? The device reboots when done.')) return;
      await jpost('/api/ota/url', { url: r.asset_url });
      setBar(-1);
      $('#ota-msg').textContent = 'Update started...';
      const res = await pollOta($('#ota-msg'));
      if (res.rebooting) {
        finishOta(await waitForReboot($('#ota-msg')));
      } else {
        toast(res.lastResult || 'Update finished',
              /fail|err|abort|invalid/i.test(res.lastResult || ''));
      }
    });
  } else {
    $('#ota-check-msg').textContent =
      'Up to date: running ' + r.current + ', latest release is ' + r.latest;
  }
});

// ---- backup / restore ------------------------------------------------------

$('#cfg-export').onclick = () => act($('#cfg-export'), async () => {
  // fetch + blob instead of a plain navigation so the X-Api-Key header rides
  // along (the export endpoint requires the key when one is set).
  const res = await window.apiFetch('/api/config/export');
  if (!res.ok) throw new Error('HTTP ' + res.status);
  const blob = await res.blob();
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'led-display-config.json';
  document.body.appendChild(a);
  a.click();
  a.remove();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  toast('Config downloaded');
});

$('#cfg-import').onclick = () => act($('#cfg-import'), async () => {
  const f = $('#cfg-file').files[0];
  if (!f) { toast('Choose a config file first', true); return; }
  if (!confirm('Apply this configuration? Current settings are replaced.')) return;
  const text = await f.text();
  const j = await jfetch('/api/config/import', { method: 'POST', body: text });
  const list = v => Array.isArray(v) ? v.join(', ') : v;
  const applied = j && j.applied !== undefined ? list(j.applied) : 'all sections';
  const skipped = j && j.skipped !== undefined ? list(j.skipped) : '';
  $('#cfg-msg').textContent = 'Applied: ' + applied + (skipped ? ' - skipped: ' + skipped : '');
  toast('Config restored');
  init();  // re-pull status/colours so the UI reflects the imported config
});

// ---- header panel simulation -------------------------------------------------
// A live mimic of the panel in the header: the same text, colour and
// scrolling state, drawn LED by LED with the firmware's 5x7 font (hex table
// generated from fonts.c). The device may be using another font; the
// simulation always draws 5x7 scaled to the panel height, which is close
// enough to read at a glance. Not an image - just the content.

const SIM_FONT_HEX =
  '000000000000005f00000007000700147f147f14242a7f2a1223130864623649552250' +
  '0005030000001c2241000041221c00082a1c2a0808083e080800503000000808080808' +
  '006060000020100804023e5149453e00427f400042615149462141454b311814127f10' +
  '27454545393c4a49493001710905033649494936064949291e00363600000056360000' +
  '0008142241141414141441221408000201510906324979413e7e1111117e7f49494936' +
  '3e414141227f4141221c7f494949417f090901013e414151327f0808087f00417f4100' +
  '2040413f017f081422417f404040407f0204027f7f0408107f3e4141413e7f09090906' +
  '3e4151215e7f09192946464949493101017f01013f4040403f1f2040201f7f2018207f' +
  '631408146303047804036151494543007f4141000204081020004141' +
  '7f0004020102044040404040000102040020545454787f4844443838444444203844' +
  '44487f3854545418087e0901020c5252523e7f0804047800447d40002040443d007f10' +
  '28440000417f40007c041804787c0804047838444444387c14141408081414187c7c08' +
  '0404084854545420043f4440203c4040207c1c2040201c3c4030403c44281028440c50' +
  '50503c4464544c44000836410000007f000000413608000804081008';

const SIM_SPACE_W = 3;   // fonts.c: 5x7 space_w
const SIM_THIN_W  = 2;   // space_w - 1, mirrors FONT_THIN_SPACE

function simGlyph(ch) {
  let code = ch.charCodeAt(0);
  if (code < 0x20 || code > 0x7e) code = 0x3f;   // '?'
  const o = (code - 0x20) * 10;
  const cols = [];
  for (let c = 0; c < 5; c++) {
    cols.push(parseInt(SIM_FONT_HEX.substr(o + c * 2, 2), 16));
  }
  return cols;
}

// Mirrors font_glyph_span: proportional trims blank columns, digits keep the
// full cell, space/thin-space are fixed-width blanks.
function simSpan(ch, prop) {
  if (ch === '\u001f') return { start: 0, width: SIM_THIN_W, blank: true };
  if (!prop) return { start: 0, width: 5 };
  if (ch >= '0' && ch <= '9') return { start: 0, width: 5 };
  const g = simGlyph(ch);
  let first = -1, last = -1;
  for (let i = 0; i < 5; i++) if (g[i]) { if (first < 0) first = i; last = i; }
  if (first < 0) return { start: 0, width: SIM_SPACE_W, blank: true };
  return { start: first, width: last - first + 1 };
}

function simTextWidth(text, prop, scale) {
  let w = 0;
  for (const ch of text) w += (simSpan(ch, prop).width + 1) * scale;
  return Math.max(0, w - scale);
}

// What the animation loop draws; refreshed from every /api/status payload.
const sim = { text: '', color: '#ffb347', mode: 'off', speed: 50, scale: 1,
              prop: true, W: 32, H: 8, offset: 0, lastStep: 0 };

function simUpdateFromStatus(s) {
  const d = s.display || {};
  sim.W = (s.layout && s.layout.width) || 32;
  sim.H = (s.layout && s.layout.height) || 8;
  const newText = d.text || '';
  const newMode = d.scroll_mode || (d.scroll ? 'restart' : 'off');
  if (newText !== sim.text || newMode !== sim.mode) sim.offset = sim.W;
  sim.text = newText;
  sim.mode = newMode;
  sim.speed = typeof d.speed === 'number' ? d.speed : 50;
  sim.prop = d.spacing !== 'monospace';
  // 5x7 stand-in for every font: clamp the scale to what fits the height.
  sim.scale = Math.max(1, Math.min(d.scale || 1, Math.floor(sim.H / 7) || 1));
  if (d.color) sim.color = rgb2hex(d.color);

  const cv = $('#hdrsim');
  const pw = sim.W * 4, ph = sim.H * 4;   // 3 px LED + 1 px gap
  if (cv.width !== pw || cv.height !== ph) { cv.width = pw; cv.height = ph; }
  // Fixed CSS height, width follows the panel's aspect ratio (capped so a
  // very wide layout cannot crowd the header).
  const cssH = 40;
  cv.style.height = cssH + 'px';
  cv.style.width = Math.min(280, Math.round(cssH * sim.W / sim.H)) + 'px';
}

function simDrawRun(ctx, x0, yOff) {
  let cursor = x0;
  for (const ch of sim.text) {
    const span = simSpan(ch, sim.prop);
    if (!span.blank && cursor + span.width * sim.scale > 0 && cursor < sim.W) {
      const g = simGlyph(ch);
      for (let c = 0; c < span.width; c++) {
        const bits = g[span.start + c];
        for (let r = 0; r < 7; r++) {
          if (!(bits & (1 << r))) continue;
          for (let dy = 0; dy < sim.scale; dy++) {
            for (let dx = 0; dx < sim.scale; dx++) {
              const x = cursor + c * sim.scale + dx;
              const y = yOff + r * sim.scale + dy;
              if (x >= 0 && x < sim.W && y >= 0 && y < sim.H) {
                ctx.fillRect(x * 4, y * 4, 3, 3);
              }
            }
          }
        }
      }
    }
    cursor += (span.width + 1) * sim.scale;
  }
}

function simFrame(now) {
  requestAnimationFrame(simFrame);
  if (document.hidden) return;
  const cv = $('#hdrsim');
  const ctx = cv.getContext('2d');

  // Advance the scroll on the firmware's speed curve (speed_to_period).
  const period = Math.max(15, 200 - sim.speed * (200 - 15) / 255);
  const scrolling = sim.mode !== 'off' && sim.text !== '';
  if (scrolling && now - sim.lastStep >= period) {
    sim.lastStep = now;
    sim.offset--;
    const w = simTextWidth(sim.text, sim.prop, sim.scale);
    const span = w + 2 * 6 * sim.scale;   // marquee gap: 2 cells
    if (sim.mode === 'continuous') {
      if (sim.offset <= -span) sim.offset += span;
    } else if (sim.offset < -w) {
      sim.offset = sim.W;
    }
  }

  ctx.fillStyle = '#000';
  ctx.fillRect(0, 0, cv.width, cv.height);
  ctx.fillStyle = '#17171b';   // unlit LEDs, faint
  for (let y = 0; y < sim.H; y++) {
    for (let x = 0; x < sim.W; x++) ctx.fillRect(x * 4, y * 4, 3, 3);
  }
  if (sim.text === '') return;   // dark panel

  ctx.fillStyle = sim.color;
  const yOff = Math.floor((sim.H - 7 * sim.scale) / 2);
  if (!scrolling) {
    const w = simTextWidth(sim.text, sim.prop, sim.scale);
    simDrawRun(ctx, w < sim.W ? Math.floor((sim.W - w) / 2) : 0, yOff);
  } else if (sim.mode === 'continuous') {
    const w = simTextWidth(sim.text, sim.prop, sim.scale);
    const span = w + 2 * 6 * sim.scale;
    let x = sim.offset;
    while (x + w > 0) x -= span;
    for (x += span; x < sim.W; x += span) simDrawRun(ctx, x, yOff);
  } else {
    simDrawRun(ctx, sim.offset, yOff);
  }
}
requestAnimationFrame(simFrame);

// ---- header + init -----------------------------------------------------------

// Human-readable connection state in the header. `s` is the /api/status
// payload, or null when the device did not answer.
function renderHeader(s) {
  const ico = $('#hdrwifi'), net = $('#hdrnet'), ip = $('#hdrip');
  // SVG className is read-only; setAttribute works everywhere.
  const state = c => ico.setAttribute('class', 'wifi ' + c);
  if (!s) {
    state('bad');
    net.textContent = 'Unreachable';
    ip.textContent = '';
    $('#hdrconn').title = 'No response from the device - check power and network';
    return;
  }
  $('#hdrfw').textContent = 'v' + s.system.fw;
  const w = s.wifi;
  if (w.mode === 'ap') {
    state('ap');
    net.textContent = 'Setup AP' + (w.ssid ? ' "' + w.ssid + '"' : '');
    ip.textContent = w.ip || '';
    $('#hdrconn').title = 'The device runs its own setup network - configure WiFi in the Network tab';
  } else if (w.connected) {
    state('ok');
    net.textContent = w.ssid || 'WiFi';
    ip.textContent = w.ip || '';
    $('#hdrconn').title = 'Connected to ' + (w.ssid || 'WiFi')
      + (w.rssi ? ' (' + w.rssi + ' dBm)' : '');
  } else {
    state('bad');
    net.textContent = 'Not connected';
    ip.textContent = '';
    $('#hdrconn').title = 'The device is up but has no WiFi link';
  }
}

function applyStatus(s) {
  lastStatus = s;
  renderHeader(s);
  simUpdateFromStatus(s);
  setDeviceTitle(s.system.device_name, !!s.system.device_name_stored);
  setBrightnessAuto(sensorDriven(s));
  $('#foot').textContent = 'led-display v' + s.system.fw;
  const cap = s.system.brightness_cap || 255;
  const br = $('#brightness');
  br.max = Math.max(1, cap);
  capSchedDim(cap);
  if (s.display && s.display.scale) {
    wantSize = (s.display.font || '5x7') + ':' + s.display.scale;
  }
  buildSizeOptions((s.layout && s.layout.height) || 16);
  if (s.display) {
    if (s.display.source) setSourceUi(s.display.source);
    // The box holds the STATIC text; in integration mode display.text is the
    // pushed value and must not leak into it.
    const boxText = s.display.text_stored !== undefined
      ? s.display.text_stored : s.display.text;
    if (boxText) $('#text').value = boxText;
    if (s.display.color) $('#color').value = rgb2hex(s.display.color);
    // The slider is an input bound to the STORED level, not to whatever the
    // arbiter currently applies (0 or the dim level in a scheduled off-window,
    // halved by the staleness guard) - Apply posts it straight back. Only while
    // the sensor drives the brightness is it a readout, and then Apply skips it.
    const seed = (!sensorDriven(s) && typeof s.display.brightness_stored === 'number')
      ? s.display.brightness_stored : s.display.brightness;
    br.value = Math.min(seed, cap);
    $('#brval').textContent = br.value;
    // Older payloads only carry the scroll bool; map it like the firmware.
    const mode = s.display.scroll_mode || (s.display.scroll ? 'restart' : 'off');
    $('#scrollmode').value = mode;
    setScrollUi();
    $('#speed').value = s.display.speed;
    $('#spval').textContent = s.display.speed;
    if (wantSize) $('#size').value = wantSize;
    if (s.display.spacing) $('#spacing').value = s.display.spacing;
  }
  $('#brcap').value = cap;
  updateCurl();
}

async function init() {
  try {
    applyStatus(await jget('/api/status'));
  } catch (e) {
    renderHeader(null);
  }
  loadColors();
}

setAdvanced(localStorage.getItem('advanced') === '1');
init();

// Light background refresh: header state, plus the WireGuard status line when
// the Network tab is open. Failures are expected during reboots.
setInterval(async () => {
  try {
    const s = await jget('/api/status');
    lastStatus = s;
    renderHeader(s);
    simUpdateFromStatus(s);
    setDeviceTitle(s.system.device_name, !!s.system.device_name_stored);
    if (s.display && s.display.scale) {
      wantSize = (s.display.font || '5x7') + ':' + s.display.scale;
    }
    buildSizeOptions((s.layout && s.layout.height) || 16);
    // While the sensor drives the brightness the slider is a readout, so keep
    // it in step with the level the device actually settled on.
    const auto = sensorDriven(s);
    setBrightnessAuto(auto);
    if (auto && s.display && typeof s.display.brightness === 'number') {
      $('#brightness').value = s.display.brightness;
      $('#brval').textContent = s.display.brightness;
    }
    if (!$('#tab-network').classList.contains('hidden')) {
      renderWgStatus(s.wireguard);
    }
  } catch (e) {
    renderHeader(null);
  }
}, 15000);
