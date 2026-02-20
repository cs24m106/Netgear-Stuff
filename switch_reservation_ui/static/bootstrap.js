/**
 * bootstrap.js — full replacement
 * - Minimal DOM updates to prevent twitching
 * - Toasts for non-blocking user feedback
 * - Theme toggle persistence
 * - Reserve / Release / Health / Mgmt-IP / Console copy support
 *
 * Assumes the HTML and theme.css provided earlier:
 * - #devices_table tbody exists
 * - #themeToggle, #refreshAll, #downloadCsv exist
 *
 * Endpoints used (update if your backend uses different paths):
 * - GET  /api/devices         -> returns array of device objects
 * - POST /api/config          -> { mgmt_ip, port_id } => returns config ports/ips
 * - POST /api/remove_mgmt_ip  -> { device_id }
 * - POST /api/refresh_health  -> { device_id } => { status: "up"/"down"/... }
 * - POST /api/reserve         -> reserve payload
 * - POST /api/release         -> release payload
 */

/* =========================
   Configuration
   ========================= */
const API = {
  devices: '/api/devices',
  config: '/api/config',
  removeMgmtIp: '/api/remove_mgmt_ip',
  refreshHealth: '/api/refresh_health',
  reserve: '/api/reserve',
  release: '/api/release'
};

const POLL_INTERVAL_MS = 2000;      // how often we poll devices (you can increase)
const UPDATE_DEBOUNCE_MS = 180;     // coalesce backend bursts into a single update
const ROW_ID_PREFIX = 'row-';       // used for each tr id

/* =========================
   Utility helpers
   ========================= */

/** create element helper: ce(tag, attrs, ...children)
 * attrs: object with attributes and special keys:
 * - class: className string
 * - text: sets textContent
 * - html: sets innerHTML
 */
function ce(tag, attrs = {}, ...children) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (k === 'class') el.className = v;
    else if (k === 'text') el.textContent = v;
    else if (k === 'html') el.innerHTML = v;
    else if (k === 'style') el.setAttribute('style', v);
    else el.setAttribute(k, v);
  }
  for (const ch of children) {
    if (ch == null) continue;
    if (typeof ch === 'string' || typeof ch === 'number') el.appendChild(document.createTextNode(String(ch)));
    else el.appendChild(ch);
  }
  return el;
}

/** simple fetch helpers */
async function getJSON(url, opts = {}) {
  const resp = await fetch(url, Object.assign({ cache: 'no-store' }, opts));
  if (!resp.ok) throw new Error(`GET ${url} -> ${resp.status}`);
  return resp.json();
}

async function postJSON(url, body = {}) {
  const resp = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
    cache: 'no-store'
  });
  const text = await resp.text();
  // sometimes backend returns plain text; try parse
  try { return JSON.parse(text); } catch (e) { return text ? { text } : {}; }
}

/** copy to clipboard (graceful fallback) */
function copyToClipboard(text) {
  if (!text) return false;
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text).catch(() => {});
    return true;
  }
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.style.position = 'fixed';
  ta.style.left = '-9999px';
  document.body.appendChild(ta);
  ta.select();
  try { document.execCommand('copy'); } catch (e) {}
  ta.remove();
  return true;
}

/* =========================
   Toast / notifications (Bootstrap based)
   ========================= */
const TOAST_CONTAINER_ID = 'app-toast-container';
function ensureToastContainer() {
  let c = document.getElementById(TOAST_CONTAINER_ID);
  if (c) return c;
  // Centered at the top
  c = ce('div', {
    id: TOAST_CONTAINER_ID,
    style: 'position:fixed; top:1rem; left:50%; transform:translateX(-50%); z-index:1080; display:flex; flex-direction:column; gap:0.5rem; align-items:center;'
  });
  document.body.appendChild(c);
  return c;
}
function showToast(title, message, opts = {}) {
  const container = ensureToastContainer();
  const toastEl = ce('div', { 
    class: 'toast align-items-center', 
    role: 'status', 
    'aria-live': 'polite', 
    'aria-atomic': 'true', 
    style: 'min-width: 220px;'
  });
  const inner = ce('div', { class: 'd-flex' });
  const body = ce('div', { class: 'toast-body' });
  body.appendChild(ce('div', { class: 'fw-bold', text: title }));
  body.appendChild(ce('div', { text: message }));
  inner.appendChild(body);
  const btnClose = ce('button', { 
    type: 'button', 
    class: 'btn-close btn-close-white me-2 m-auto', 
    'aria-label': 'Close' 
  });
  btnClose.addEventListener('click', () => { 
    if (bsToast) bsToast.hide(); 
    toastEl.remove(); 
  });
  inner.appendChild(btnClose);
  toastEl.appendChild(inner);
  container.appendChild(toastEl);

  // Style by type
  const type = opts.type || 'info';
  if (type === 'success') body.style.backgroundColor = 'rgba(40,167,69,0.12)';
  if (type === 'error') body.style.backgroundColor = 'rgba(220,53,69,0.12)';

  // Use Bootstrap toast with proper autohide
  let bsToast = null;
  const autohide = opts.autohide !== false; // Default to true
  const delay = typeof opts.autohide === 'number' ? opts.autohide : 3500;
  
  try {
    bsToast = new bootstrap.Toast(toastEl, { 
      autohide: autohide, 
      delay: delay 
    });
    bsToast.show();
    toastEl.addEventListener('hidden.bs.toast', () => toastEl.remove());
  } catch (e) {
    // Fallback: remove after timeout
    setTimeout(() => toastEl.remove(), delay);
  }
}

/* =========================
   Bootstrap helper: apply minimal classes to custom buttons
   ========================= */
function bootstrapifyButtons(root) {
  if (!root) return;
  // find all icon-only pseudo buttons (they may be <button> already) and set small classes
  root.querySelectorAll('.icon-only').forEach(btn => {
    if (!btn.classList.contains('btn')) btn.classList.add('btn', 'btn-outline-secondary', 'btn-sm');
  });
  // action-reserve/release buttons
  root.querySelectorAll('.action-reserve').forEach(b => {
    if (!b.classList.contains('btn')) b.classList.add('btn', 'btn-sm', 'btn-primary');
  });
  root.querySelectorAll('.action-release').forEach(b => {
    if (!b.classList.contains('btn')) b.classList.add('btn', 'btn-sm', 'btn-outline-secondary');
  });
}

/* =========================
   Theme toggle (persist in localStorage)
   ========================= */
const THEME_KEY = 'switchmgr.theme';
function setTheme(theme) {
  if (theme === 'dark') document.body.setAttribute('data-theme', 'dark');
  else document.body.removeAttribute('data-theme');
  try { localStorage.setItem(THEME_KEY, theme); } catch (e) {}
}
function toggleTheme() {
  const cur = (document.body.getAttribute('data-theme') === 'dark') ? 'dark' : 'light';
  const next = cur === 'dark' ? 'light' : 'dark';
  setTheme(next);
  // do not show a toast for successful theme switch — only show toasts on errors
  // optional: console feedback for devs
  console.info('Theme switched to', next);
}
(function initTheme() {
  try {
    const saved = localStorage.getItem(THEME_KEY) || 'light';
    setTheme(saved);
    const btn = document.getElementById('themeToggle');
    if (btn) btn.addEventListener('click', toggleTheme);
  } catch (e) { /* ignore */ }
})();

/* =========================
   Core: minimal update strategy
   ========================= */

let prevDevices = new Map(); // device_id -> key string
let updateTimer = null;
let isUpdating = false;

/**
 * deviceKey(d): produce a JSON string of the small set of fields we care about.
 * Keep this deterministic and stable across updates — used to detect changed rows.
 */
function deviceKey(d) {
  // include fields that affect DOM:
  return JSON.stringify({
    device_id: d.device_id,
    model_name: d.model_name,
    hw_id: d.hw_id,
    tag: d.tag,
    mgmt_ip: d.mgmt_ip,
    port_id: d.port_id,
    health: d.health,
    current_user: d.current_user,
    duration: d.duration,
    resv_block: d.resv_block
  });
}

/* Get tbody */
function tbody() {
  return document.querySelector('#devices_table tbody');
}

/* createRowForDevice: builds a full <tr> for a device.
   Column order MUST match the HTML table header.
   Columns:
     0: Id
     1: Model
     2: Hardware
     3: UI
     4: Mgmt-Ip
     5: Console
     6: Health
     7: Status
     8: Reservation
     9: Actions
*/
function createRowForDevice(d) {
  // helper: text or fallback
  const safe = (v, fallback = '') => (v === null || v === undefined) ? fallback : v;

  const tr = ce('tr', { id: ROW_ID_PREFIX + d.device_id });

  // 0: Id
  tr.appendChild(ce('td', { class: 'col-left', text: String(safe(d.device_id)) }));

  // 1: Model
  tr.appendChild(ce('td', { class: 'col-left word-wrap', text: safe(d.model_name, '') }));

  // 2: Hardware
  tr.appendChild(ce('td', { class: 'col-left', text: safe(d.hw_id, '') }));

  // 3: UI (stack of links) - placeholder; filled asynchronously from /api/config
  const tdUI = ce('td', { class: 'col-center' }, ce('div', { class: 'ui-stack', text: '— — —' }));
  tr.appendChild(tdUI);

  // 4: Mgmt-Ip (text + actions)
  const tdMgmt = ce('td', { class: 'col-left' });
  const mgmtWrap = ce('div', { class: 'cell-flex mgmt-actions' });
  const mgmtText = ce('span', { text: safe(d.mgmt_ip, '—.—.—.—') });
  mgmtWrap.appendChild(mgmtText);

  if (d.mgmt_ip) {
    const removeBtn = ce('button', { type: 'button', class: 'icon-only', title: 'Remove Mgmt-IP' }, '✖');
    removeBtn.addEventListener('click', async (ev) => {
      ev.preventDefault();
      try {
        const res = await postJSON(API.removeMgmtIp, { device_id: d.device_id });
        showToast('Mgmt IP', 'Mgmt-IP remove requested', { type: 'info', autohide: 1400 });
        // trigger immediate refresh for this device
        pollAndUpdate(true);
      } catch (err) {
        showToast('Error', 'Failed to remove mgmt-ip', { type: 'error' });
        console.error(err);
      }
    });

    const copyBtn = ce('button', { type: 'button', class: 'icon-only', title: `ssh admin@${d.mgmt_ip}` }, '⧉');
    copyBtn.addEventListener('click', () => {
      copyToClipboard(`ssh admin@${d.mgmt_ip}`);
      showToast('Copied', `ssh admin@${d.mgmt_ip}`, { autohide: 1200, type: 'info' });
    });

    mgmtWrap.appendChild(removeBtn);
    mgmtWrap.appendChild(copyBtn);
  }
  tdMgmt.appendChild(mgmtWrap);
  tr.appendChild(tdMgmt);

  // 5: Console (port + copy telnet if available)
  const tdConsole = ce('td', { class: 'col-left' });
  const consoleWrap = ce('div', { class: 'cell-flex console-actions' });
  const portSpan = ce('span', { text: (d.port_id == null || d.port_id === '') ? '-' : String(d.port_id) });
  consoleWrap.appendChild(portSpan);
  tdConsole.appendChild(consoleWrap);
  tr.appendChild(tdConsole);

  // 6: Health (text + refresh)
  const tdHealth = ce('td', { class: 'col-left' });
  const healthWrap = ce('div', { class: 'cell-flex' });
  const healthSpan = ce('span', { text: safe(d.health, 'unk') });
  // initial class mapping
  const healthClass = mapHealthClass(d.health);
  healthSpan.className = healthClass;
  healthWrap.appendChild(healthSpan);

  const healthRefresh = ce('button', { type: 'button', class: 'icon-only', title: 'Refresh' }, '↻');
  healthRefresh.addEventListener('click', async () => {
    try {
      healthSpan.innerText = '...';
      // Match main.js logic
      const j = await postJSON(API.refreshHealth, { device_id: d.device_id });
      
      // Support both {ok, status} and {status} response formats
      const newStatus = (j.status || j.state || 'unk').toLowerCase();
      healthSpan.innerText = newStatus;
      
      // Apply classes directly as requested
      if (newStatus.includes('up')) healthSpan.className = 'health-up';
      else if (newStatus.includes('down')) healthSpan.className = 'health-down';
      else if (newStatus.includes('busy')) healthSpan.className = 'health-busy';
      else healthSpan.className = 'health-unk';

    } catch (err) {
      healthSpan.innerText = 'err';
      healthSpan.className = 'health-unk';
      showToast('Error', 'Health refresh failed', { type: 'error' });
    }
  });
  healthWrap.appendChild(healthRefresh);
  tdHealth.appendChild(healthWrap);
  tr.appendChild(tdHealth);

  // 7: Status (chip)
  const tdStatus = ce('td', { class: 'col-center' });
  const statusSpan = ce('span', {});
  setStatusSpanForTag(statusSpan, d.tag);
  tdStatus.appendChild(statusSpan);
  tr.appendChild(tdStatus);

  // 8: Reservation - input area or static info
  const tdResv = ce('td', { class: 'col-left' });
  // create placeholder and let setupReservation populate it
  tdResv.appendChild(ce('div', { class: 'resv-container' }));
  tr.appendChild(tdResv);

  // 9: Actions - buttons area
  const tdActions = ce('td', { class: 'col-center' });
  tdActions.appendChild(ce('div', { class: 'actions-wrap' }));
  tr.appendChild(tdActions);

  // apply bootstrap styles
  bootstrapifyButtons(tr);

  // populate UI stack + console telnet by calling config endpoint asynchronously
  (async function populateConfig(mgmt_ip, port_id) {
    if (!mgmt_ip || mgmt_ip === '-' || mgmt_ip === '—.—.—.—') return;
    try {
      const cfg = await postJSON(API.config, { mgmt_ip, port_id });
      if (!cfg) return;
      // cfg expected fields: av_port, new_main_port, old_main_port, console_ip, device_port
      const uiStack = tdUI.querySelector('.ui-stack');
      uiStack.innerHTML = '';
      const links = [];
      if (cfg.av_port) links.push({ label: 'AV', href: cfg.av_port });
      if (cfg.new_main_port) links.push({ label: 'Main(new)', href: cfg.new_main_port });
      if (cfg.old_main_port) links.push({ label: 'Main(old)', href: cfg.old_main_port });
      if (links.length === 0) uiStack.innerHTML = '— — —';
      else {
        for (const l of links) {
          const a = ce('a', { href: l.href, target: '_blank', text: l.label });
          uiStack.appendChild(a);
        }
      }
      // console telnet copy
      if (cfg.console_ip && cfg.device_port && portSpan) {
        const telnet = `telnet ${cfg.console_ip} ${cfg.device_port}`;
        const tbtn = ce('button', { type: 'button', class: 'icon-only', title: telnet }, '⧉');
        tbtn.addEventListener('click', () => {
          copyToClipboard(telnet);
          showToast('Copied', telnet, { autohide: 1200 });
        });
        const consWrap = tdConsole.querySelector('.console-actions');
        consWrap.appendChild(tbtn);
        bootstrapifyButtons(consWrap);
      }
    } catch (err) {
      // ignore config failures but log
      console.debug('config fetch failed for', d.device_id, err);
    }
  })(d.mgmt_ip, d.port_id);

  // Reservation & Action wiring - separated to allow update without losing editing state
  setupReservationAndActions(tr, d);

  return tr;
}

/* map health text -> css class */
function mapHealthClass(h) {
  if (!h) return 'health-unk';
  const s = String(h).toLowerCase();
  if (s.includes('up')) return 'health-up';
  if (s.includes('down') || s.includes('fail')) return 'health-down';
  if (s.includes('busy') || s.includes('loading')) return 'health-busy';
  return 'health-unk';
}

/* set statusSpan based on tag string */
function setStatusSpanForTag(span, tag) {
  const t = (tag || 'free').toLowerCase();
  span.className = t === 'free' ? 'status-free' : (t === 'resv' ? 'status-resv' : (t === 'static' ? 'status-static' : ''));
  span.innerText = t === 'free' ? 'Free' : (t === 'resv' ? 'Reserved' : (t === 'static' ? 'Static' : String(tag || '')));
}

/* setupReservationAndActions(tr, d)
  Creates the reservation UI in column 8 and actions in column 9.
  Preserves inputs if user is editing them (avoid destroying inputs while focused).
  Cyclic 0-24 hrs / 0-59 mins logic. Shows RELEASE button for both 'resv' and 'static' tags.
 */
function setupReservationAndActions(tr, d) {
  const cells = tr.cells;
  const tdResv = cells[8];
  const tdActions = cells[9];
  
  // Prevent rewriting inputs if user is currently focused/editing
  const isEditing = tdResv.querySelector('input:focus') != null || tdResv.getAttribute('data-editing') === '1';
  if (isEditing) return;
  
  // Clear previous content
  tdResv.innerHTML = '';
  tdActions.innerHTML = '';
  
  const tag = (d.tag || 'free').toLowerCase();
  
  if (tag === 'free') {
    // --- Reservation Column gets INPUTS (CENTERED) ---
    const container = ce('div', { class: 'resv-container' });
    const inputsDiv = ce('div', { class: 'resv-inputs' });
    
    const userInp = ce('input', { 
      type: 'text', 
      class: 'input-user', 
      placeholder: 'Username', 
      value: d.current_user || '' 
    });

    const timeRow = ce('div', { class: 'resv-row' });

    const hrsInp = ce('input', { type: 'number', class: 'input-compact', value: 1 });
    hrsInp.addEventListener('input', function() {
      let val = parseInt(this.value);
      if (val < 0) this.value = 24;
      else if (val > 24) this.value = 0;
    });

    const minsInp = ce('input', { type: 'number', class: 'input-compact', value: 0 });
    minsInp.addEventListener('input', function() {
      let val = parseInt(this.value);
      if (val < 0) this.value = 59;
      else if (val > 59) this.value = 0;
    });

    timeRow.appendChild(hrsInp);
    timeRow.appendChild(ce('span', { class: 'meta-small', text: 'hrs' }));
    timeRow.appendChild(minsInp);
    timeRow.appendChild(ce('span', { class: 'meta-small', text: 'mins' }));

    inputsDiv.appendChild(userInp);
    inputsDiv.appendChild(timeRow);
    container.appendChild(inputsDiv);
    tdResv.appendChild(container);

    // Prevent updates while typing
    [userInp, hrsInp, minsInp].forEach(el => {
      el.addEventListener('focus', () => tdResv.setAttribute('data-editing', '1'));
      el.addEventListener('blur', () => setTimeout(() => tdResv.removeAttribute('data-editing'), 200));
    });

    // --- Actions Column gets RESERVE BUTTON ---
    const reserveBtn = ce('button', { class: 'btn btn-sm action-reserve', text: 'RESERVE' });
    reserveBtn.onclick = async () => {
      const user = userInp.value.trim();
      const hrs = parseInt(hrsInp.value || '0', 10);
      const mins = parseInt(minsInp.value || '0', 10);
      
      if (!user) { showToast('Error', "Please enter a user name", { type: 'error', autohide: 2500 }); return; }
      
      tdResv.setAttribute('data-editing', '1'); 
      try {
        const r = await postJSON(API.reserve, { device_id: d.device_id, user, hours: hrs, minutes: mins });
        if (r && (r.ok || r.status === 'success')) {
          showToast('Reserve', 'Device reserved successfully', { type: 'success', autohide: 2500 });
        }
        pollAndUpdate(true);
      } catch (err) {
        showToast('Error', 'Reserve failed', { type: 'error', autohide: 3500 });
      } finally {
        tdResv.removeAttribute('data-editing');
      }
    };
    tdActions.appendChild(reserveBtn);
    
  } else {
    // --- Reservation Column gets INFO for 'resv' or 'static' (LEFT-ALIGNED) ---
    const pre = ce('pre', {
      style: 'margin: 0; font-family: monospace; text-align: left;',
      text: d.resv_block || (tag === 'static' ? 'Static assignment' : '')
    });
    tdResv.appendChild(pre);
    
    // --- Actions Column gets RELEASE BUTTON ---
    const releaseBtn = ce('button', { class: 'btn btn-sm action-release', text: 'RELEASE' });
    releaseBtn.onclick = async () => {
      try {
        const r = await postJSON(API.release, { device_id: d.device_id });
        if (r && (r.ok || r.status === 'success')) {
          showToast('Release', 'Device released successfully', { type: 'success', autohide: 2500 });
        }
        pollAndUpdate(true);
      } catch (err) {
        showToast('Error', 'Release failed', { type: 'error', autohide: 3500 });
      }
    };
    tdActions.appendChild(releaseBtn);
  }
}

/* =========================
   Minimal update: updateRow(d)
   Only change DOM nodes that actually differ to avoid focus loss and layout thrash
   ========================= */
function updateRow(d) {
  const id = ROW_ID_PREFIX + d.device_id;
  const tr = document.getElementById(id);
  if (!tr) return;

  const cells = tr.cells;
  // 0: id - static
  // 1: model
  if (cells[1] && cells[1].innerText !== (d.model_name || '')) cells[1].innerText = d.model_name || '';

  // 2: hardware
  if (cells[2] && cells[2].innerText !== (d.hw_id || '')) cells[2].innerText = d.hw_id || '';

  // 4: mgmt ip (span)
  if (cells[4]) {
    const mgmtSpan = cells[4].querySelector('span');
    const newMgmt = d.mgmt_ip || '—.—.—.—';
    if (mgmtSpan && mgmtSpan.innerText !== newMgmt) mgmtSpan.innerText = newMgmt;
    // If mgmt ip newly present or removed, we re-create mgmt actions (safe small update)
    const hasMgmtBtn = !!cells[4].querySelector('.icon-only');
    if (d.mgmt_ip && !hasMgmtBtn) {
      // re-run a lightweight update: append copy/remove
      const mgmtWrap = cells[4].querySelector('.mgmt-actions') || cells[4].querySelector('.cell-flex');
      if (mgmtWrap) {
        // remove existing icon-only elements to avoid duplicates
        mgmtWrap.querySelectorAll('.icon-only').forEach(n => n.remove());
        const removeBtn = ce('button', { type: 'button', class: 'icon-only', title: 'Remove Mgmt-IP' }, '✖');
        removeBtn.addEventListener('click', async (ev) => {
          ev.preventDefault();
          try {
            await postJSON(API.removeMgmtIp, { device_id: d.device_id });
            // do not show success toast -- table will refresh and reflect the removal
            pollAndUpdate(true);
          } catch (err) {
            showToast('Error', 'Failed to remove mgmt-ip', { type: 'error' });
            console.error(err);
          }
        });
        const copyBtn = ce('button', { type: 'button', class: 'icon-only', title: `ssh admin@${d.mgmt_ip}` }, '⧉');
        copyBtn.addEventListener('click', () => { copyToClipboard(`ssh admin@${d.mgmt_ip}`); showToast('Copied', `ssh admin@${d.mgmt_ip}`); });
        mgmtWrap.appendChild(removeBtn);
        mgmtWrap.appendChild(copyBtn);
        bootstrapifyButtons(mgmtWrap);
      }
    }
  }

  // 5: console port
  if (cells[5]) {
    const portSpan = cells[5].querySelector('span');
    const newPort = (d.port_id == null || d.port_id === '') ? '-' : String(d.port_id);
    if (portSpan && portSpan.innerText !== newPort) portSpan.innerText = newPort;
  }

  // 6: health
  if (cells[6]) {
    const hspan = cells[6].querySelector('span');
    const newHealth = (d.health || 'unk');
    if (hspan && hspan.innerText !== newHealth) {
      hspan.innerText = newHealth;
      hspan.className = mapHealthClass(newHealth);
    }
  }

  // 7: status
  if (cells[7]) {
    const stCell = cells[7];
    const cur = stCell.innerText.trim().toLowerCase();
    const want = ((d.tag || 'free') === 'free' ? 'free' : ((d.tag || '') === 'resv' ? 'reserved' : (d.tag || '')));
    if (!cur || cur !== want) {
      stCell.innerHTML = '';
      const span = ce('span', {});
      setStatusSpanForTag(span, d.tag);
      stCell.appendChild(span);
    }
  }

  // 8 & 9: reservation + actions - re-setup to reflect changed tag/user
  // But avoid overwriting inputs if user editing (check focus)
  const tdResv = cells[8];
  const isEditing = tdResv && (tdResv.querySelector('input:focus') != null || tdResv.getAttribute('data-editing') === '1');
  if (!isEditing) {
    setupReservationAndActions(tr, d);
  }
}

/* =========================
   Polling & update orchestration
   - pollAndUpdate(force) fetches device list and diffs
   - debounced to coalesce frequent responses
   ========================= */
async function fetchDevices() {
  try {
    const resp = await getJSON(API.devices);
    // Backends sometimes return either:
    //  - an array: [ {device...}, ... ]
    //  - an object: { devices: [ {...}, ... ] }
    // Support both forms to be robust.
    if (Array.isArray(resp)) {
      return resp;
    }
    if (resp && Array.isArray(resp.devices)) {
      return resp.devices;
    }
    console.warn('fetchDevices: unexpected response shape from /api/devices', resp);
    return [];
  } catch (err) {
    console.error('fetchDevices error', err);
    return [];
  }
}

/* schedule and debounce wrapper */
function scheduleUpdate(force = false) {
  if (updateTimer) clearTimeout(updateTimer);
  updateTimer = setTimeout(() => pollAndUpdate(force).catch(e => console.error(e)), UPDATE_DEBOUNCE_MS);
}

async function pollAndUpdate(force = false) {
  if (isUpdating) return;
  isUpdating = true;
  try {
    const devices = await fetchDevices();
    const tb = tbody();
    if (!tb) return;

    const keep = new Set();
    for (const d of devices) {
      const id = String(d.device_id);
      keep.add(id);
      const key = deviceKey(d);
      const existing = document.getElementById(ROW_ID_PREFIX + id);
      if (!existing) {
        const newRow = createRowForDevice(d);
        tb.appendChild(newRow);
        prevDevices.set(id, key);
      } else {
        if (force || prevDevices.get(id) !== key) {
          prevDevices.set(id, key);
          updateRow(d);
        }
      }
    }

    // remove stale rows (optional)
    Array.from(tb.querySelectorAll('tr[id^="' + ROW_ID_PREFIX + '"]')).forEach(r => {
      const rid = r.id.replace(ROW_ID_PREFIX, '');
      if (!keep.has(rid)) {
        r.remove();
        prevDevices.delete(rid);
      }
    });
  } finally {
    isUpdating = false;
  }
}

/* start polling loop */
let pollIntervalHandle = null;
function startPolling() {
  // initial immediate update
  pollAndUpdate(true);
  if (pollIntervalHandle) clearInterval(pollIntervalHandle);
  pollIntervalHandle = setInterval(() => scheduleUpdate(false), POLL_INTERVAL_MS);
}

/* manual stop */
function stopPolling() {
  if (pollIntervalHandle) clearInterval(pollIntervalHandle);
  pollIntervalHandle = null;
}

/* =========================
   Utility: Export CSV
   ========================= */
function exportDevicesToCsv(devices) {
  if (!Array.isArray(devices)) return '';
  const header = ['device_id','model_name','hw_id','mgmt_ip','port_id','tag','current_user','duration','resv_block','health'];
  const rows = [header.join(',')];
  for (const d of devices) {
    const row = header.map(h => {
      let v = d[h] == null ? '' : String(d[h]);
      v = v.replace(/"/g, '""');
      if (v.includes(',') || v.includes('"')) v = `"${v}"`;
      return v;
    }).join(',');
    rows.push(row);
  }
  return rows.join('\n');
}
async function handleDownloadCsv() {
  try {
    const devices = await fetchDevices();
    const csv = exportDevicesToCsv(devices);
    const blob = new Blob([csv], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const a = ce('a', { href: url, download: `devices-${new Date().toISOString().slice(0,10)}.csv` });
    document.body.appendChild(a);
    a.click();
    a.remove();
    URL.revokeObjectURL(url);
    showToast('Export', 'CSV downloaded', { autohide: 1500, type: 'success' });
  } catch (err) {
    showToast('Error', 'Export failed', { type: 'error' });
    console.error(err);
  }
}

/* =========================
   Wire up top-level controls
   ========================= */
(function wireTopControls() {
  const refreshBtn = document.getElementById('refreshAll');
  if (refreshBtn) {
    refreshBtn.addEventListener('click', () => {
      showToast('Refresh', 'Refreshing devices...', { autohide: 900 });
      pollAndUpdate(true);
    });
  }
  const dlBtn = document.getElementById('downloadCsv') || document.getElementById('downloadCSV') || document.getElementById('exportCsv');
  if (dlBtn) dlBtn.addEventListener('click', handleDownloadCsv);

  // wire theme toggle already in initTheme; ensure keyboard accessibility
  const tBtn = document.getElementById('themeToggle');
  if (tBtn) {
    tBtn.addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggleTheme(); } });
  }
})();

/* =========================
   Boot
   ========================= */
window.addEventListener('load', () => {
  // small guard: ensure tbody exists
  if (!document.querySelector('#devices_table tbody')) {
    console.warn('bootstrap.js: devices_table tbody not found');
    return;
  }

  // small UX: avoid focus stealing by not using alert(); all UX via toasts now
  startPolling();

  // mild safety: re-poll when document becomes visible again (useful if user switched tabs)
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible') scheduleUpdate(true);
  });
});