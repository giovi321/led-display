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

// ---- shared state ----------------------------------------------------------
// NOTE: not named "status" - a top-level var of that name would collide with
// window.status (a string-coercing browser property).

let lastStatus = null;    // last /api/status payload
let colors = [];          // saved colour presets [{name,r,g,b}]
let editorMounted = false;

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
  if (prev && prev.dataset.tab === 'hardware' && name !== 'hardware'
      && window.Editor && typeof window.Editor.leave === 'function') {
    window.Editor.leave();
  }
  $$('#tabs button').forEach(b => b.classList.toggle('active', b.dataset.tab === name));
  $$('main > section').forEach(sec => sec.classList.toggle('hidden', sec.id !== 'tab-' + name));
  if (name === 'schedule') refreshSchedule();
  if (name === 'hardware') openHardware();
  if (name === 'network') refreshNetwork();
  if (name === 'integrations') refreshIntegrations();
  if (name === 'system') refreshSystem();
}

$$('#tabs button').forEach(b => { b.onclick = () => tab(b.dataset.tab); });
$('#advtoggle').onchange = e => setAdvanced(e.target.checked);

// ---- Display tab -----------------------------------------------------------

$('#brightness').oninput = e => { $('#brval').textContent = e.target.value; };
$('#speed').oninput = e => { $('#spval').textContent = e.target.value; };

$('#apply').onclick = () => act($('#apply'), async () => {
  const body = {
    text: $('#text').value,
    color: hex2rgb($('#color').value),
    scroll: $('#scroll').checked,
    speed: +$('#speed').value,
    scale: +$('#scale').value,
  };
  await jpost('/api/display/text', body);
  await jpost('/api/display/brightness', { brightness: +$('#brightness').value });
  toast('Applied');
});

$('#clear').onclick = () => act($('#clear'), async () => {
  await jpost('/api/display/clear');
  toast('Display cleared');
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

async function refreshSchedule() {
  try {
    const s = await jget('/api/schedule');
    $('#sched-en').checked = !!s.enabled;
    $('#sched-on').value = String(s.on_hour).padStart(2, '0') + ':' + String(s.on_min).padStart(2, '0');
    $('#sched-off').value = String(s.off_hour).padStart(2, '0') + ':' + String(s.off_min).padStart(2, '0');
    $$('#sched-days input').forEach(cb => {
      cb.checked = !!(s.days & (1 << +cb.dataset.day));
    });
  } catch (e) { /* first load can race the device; keep defaults */ }
}

$('#sched-save').onclick = () => act($('#sched-save'), async () => {
  const on = $('#sched-on').value.split(':');
  const off = $('#sched-off').value.split(':');
  let days = 0;
  $$('#sched-days input').forEach(cb => { if (cb.checked) days |= (1 << +cb.dataset.day); });
  await jpost('/api/schedule', {
    enabled: $('#sched-en').checked,
    on_hour: +on[0], on_min: +on[1],
    off_hour: +off[0], off_min: +off[1],
    days: days,
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
}

$$('#tab-hardware [data-test]').forEach(b => {
  b.onclick = () => act(b, async () => {
    await jpost('/api/display/test', { pattern: b.dataset.test });
    toast('Test: ' + b.dataset.test);
  });
});

$('#brcap-save').onclick = () => act($('#brcap-save'), async () => {
  let v = Math.round(+$('#brcap').value);
  if (!(v >= 1 && v <= 255)) { toast('Cap must be 1-255', true); return; }
  await jpost('/api/display/brightness_cap', { cap: v });
  // Reflect the new ceiling on the Display tab immediately.
  const br = $('#brightness');
  br.max = v;
  if (+br.value > v) { br.value = v; $('#brval').textContent = v; }
  toast('Brightness cap set to ' + v);
});

// ---- Network tab -----------------------------------------------------------

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
  box.innerHTML = nets.length ? '' : '<p class="note">No networks found.</p>';
  nets.forEach(n => {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'net';
    const ssid = document.createElement('span');
    ssid.className = 'ssid';
    ssid.textContent = n.ssid;
    const rssi = document.createElement('span');
    rssi.className = 'rssi';
    rssi.textContent = n.rssi + ' dBm';
    btn.appendChild(ssid);
    btn.appendChild(rssi);
    btn.onclick = () => { $('#wssid').value = n.ssid; $('#wpass').focus(); };
    box.appendChild(btn);
  });
});

$('#wconnect').onclick = () => act($('#wconnect'), async () => {
  const ssid = $('#wssid').value.trim();
  const password = $('#wpass').value;
  if (!ssid) { toast('Need an SSID', true); return; }
  await jpost('/api/wifi/connect', { ssid: ssid, password: password });
  toast('Saved - the device is rebooting onto ' + ssid);
});

$('#wreset').onclick = () => act($('#wreset'), async () => {
  if (!confirm('Erase WiFi credentials and reboot into setup mode?')) return;
  await jpost('/api/wifi/reset');
  toast('Resetting WiFi...');
});

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
    $('#devname').value = j.device_name || '';
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
  } catch (e) { /* endpoint not up yet; leave the form editable */ }
  updateCurl();
}

function updateCurl() {
  const host = (lastStatus && lastStatus.wifi.ip) || '<device-ip>';
  $('#curl-ex').textContent =
    "curl -X POST http://" + host + "/api/value -H 'X-Api-Key: KEY' -d '{\"value\":42}'";
}

$('#devname-save').onclick = () => act($('#devname-save'), async () => {
  await jpost('/api/integrations', { device_name: $('#devname').value.trim() });
  toast('Device name saved');
});

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

// ---- init ------------------------------------------------------------------

function applyStatus(s) {
  lastStatus = s;
  $('#hdrstatus').textContent = s.wifi.mode + ' / ' + (s.wifi.ip || '-');
  $('#foot').textContent = 'led-display v' + s.system.fw;
  const cap = s.system.brightness_cap || 255;
  const br = $('#brightness');
  br.max = Math.max(1, cap);
  if (s.display) {
    if (s.display.text) $('#text').value = s.display.text;
    if (s.display.color) $('#color').value = rgb2hex(s.display.color);
    br.value = Math.min(s.display.brightness, cap);
    $('#brval').textContent = br.value;
    $('#scroll').checked = !!s.display.scroll;
    $('#speed').value = s.display.speed;
    $('#spval').textContent = s.display.speed;
    if (s.display.scale) $('#scale').value = String(s.display.scale);
  }
  $('#brcap').value = cap;
  updateCurl();
}

async function init() {
  try {
    applyStatus(await jget('/api/status'));
  } catch (e) {
    $('#hdrstatus').textContent = 'offline';
  }
  loadColors();
}

setAdvanced(localStorage.getItem('advanced') === '1');
init();

// Light background refresh: header line, plus the WireGuard status line when
// the Network tab is open. Failures are expected during reboots - stay quiet.
setInterval(async () => {
  try {
    const s = await jget('/api/status');
    lastStatus = s;
    $('#hdrstatus').textContent = s.wifi.mode + ' / ' + (s.wifi.ip || '-');
    if (!$('#tab-network').classList.contains('hidden')) {
      renderWgStatus(s.wireguard);
    }
  } catch (e) { /* transient */ }
}, 15000);
