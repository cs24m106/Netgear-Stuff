/* =========================
   Configuration
   ========================= */
const API = {
  devicesView: '/api/devices/view',
  config: '/api/config',
  removeMgmtIp: '/api/remove_mgmt_ip',
  refreshHealth: '/api/refresh_health',
  reserve: '/api/reserve',
  release: '/api/release',
  devicesAdd: '/api/devices/add',
  devicesEdit: '/api/devices/edit',
  devicesDelete: '/api/devices/delete'
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

  // 0: Id (device_id + serial_no on separate lines)
  const idCell = ce('td', { class: 'col-left word-wrap' });
  const idContent = ce('div', { style: 'display: flex; flex-direction: column; line-height: 1.3;' });
  idContent.appendChild(ce('div', { 
    style: 'font-weight: 600; font-size: 0.95rem;', 
    text: String(safe(d.device_id)) 
  }));
  if (d.serial_no) {
    idContent.appendChild(ce('div', { 
      style: 'font-size: 0.78rem; color: var(--muted); margin-top: 0.15rem;', 
      text: String(d.serial_no) 
    }));
  }
  idCell.appendChild(idContent);
  tr.appendChild(idCell);

  // 1: Model
  tr.appendChild(ce('td', { class: 'col-left word-wrap', text: safe(d.model_name, '') }));

  // 2: Serial No (NEW COLUMN - was missing, causing misalignment)
  tr.appendChild(ce('td', { class: 'col-left', text: safe(d.serial_no, '') }));
  
  // 3: Hardware
  tr.appendChild(ce('td', { class: 'col-left', text: safe(d.hw_id, '') }));

  // 4: UI (stack of links) - placeholder; filled asynchronously from /api/config
  const tdUI = ce('td', { class: 'col-center' }, ce('div', { class: 'ui-stack', text: '— — —' }));
  tr.appendChild(tdUI);

  // 5: Mgmt-Ip (text + actions)
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
        showToast('Mgmt IP', 'Mgmt-IP remove requested\nRefrsh health to try fetch ip', { type: 'info', autohide: 1400 });
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

  // 6: Console (port + copy telnet if available)
  const tdConsole = ce('td', { class: 'col-left' });
  const consoleWrap = ce('div', { class: 'cell-flex console-actions' });
  const portSpan = ce('span', { text: (d.port_id == null || d.port_id === '') ? '-' : String(d.port_id) });
  consoleWrap.appendChild(portSpan);
  tdConsole.appendChild(consoleWrap);
  tr.appendChild(tdConsole);

  // 7: Health (text + refresh)
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

  // 8: Status (chip)
  const tdStatus = ce('td', { class: 'col-center' });
  const statusSpan = ce('span', {});
  setStatusSpanForTag(statusSpan, d.tag);
  tdStatus.appendChild(statusSpan);
  tr.appendChild(tdStatus);

  // 9: Reservation - input area or static info
  const tdResv = ce('td', { class: 'col-left' });
  // create placeholder and let setupReservation populate it
  tdResv.appendChild(ce('div', { class: 'resv-container' }));
  tr.appendChild(tdResv);

  // 10: Actions - buttons area
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
      if (cfg.av_port) links.push({ label: 'AV', href: `https://localhost:${cfg.av_port}/` });
      if (cfg.new_main_port) links.push({ label: 'Main(https)', href: `https://localhost:${cfg.new_main_port}/` });
      if (cfg.old_main_port) links.push({ label: 'Main(http)', href: `http://localhost:${cfg.old_main_port}/` });
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
  Cyclic 0-24 hrs / 0-59 mins logic. Shows RELEASE button for both 'resv' and 'static' tags.*/
function setupReservationAndActions(tr, d) {
  const cells = tr.cells;
  const tdResv = cells[8];
  const tdActions = cells[9];
  
  // Prevent rewriting inputs if user is currently focused/editing
  const isEditing = tdResv.querySelector('input:focus') != null || tdResv.getAttribute('data-editing') === '1';
  if (isEditing) return;
  
  const tag = (d.tag || 'free').toLowerCase();
  // Clear previous content
  tdResv.innerHTML = '';
  tdActions.innerHTML = '';
  
  // <<<===== Setup Reservation Field =====>>>
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
    
  } else {
    // --- Reservation Column gets INFO for 'resv' or 'static' (LEFT-ALIGNED) ---
    const pre = ce('pre', {
      style: 'margin: 0; font-family: monospace; text-align: left;',
      text: d.resv_block || (tag === 'static' ? 'Static assignment' : '')
    });
    tdResv.appendChild(pre);
  }

  // <<<===== Setup Actions Field =====>>>
  const actionsWrap = ce('div', { class: 'actions-wrap', style: 'display:flex; gap:0.25rem; justify-content:center; flex-wrap:wrap;' });

  if (tag === 'free') {
    // RESERVE button
    const reserveBtn = ce('button', { class: 'btn btn-sm action-reserve', text: 'RESERVE', type: 'button' });
    reserveBtn.onclick = async () => {
      const user = userInp.value.trim();
      const hrs = parseInt(hrsInp.value || '0', 10);
      const mins = parseInt(minsInp.value || '0', 10);
      
      if (!user) { showToast('Error', "Please enter a user name", { type: 'error', autohide: 2500 }); return; }
      
      tdResv.setAttribute('data-editing', '1'); 
      try {
        const r = await postJSON(API.reserve, { device_id: d.device_id, user, hours: hrs, minutes: mins });
        if (r && typeof r.ok !== 'undefined') {
          const toastType = r.ok ? 'success' : 'error';
          showToast('Reserve', `Reservation-Api Handled: ${r.msg || '?'}`, { type: toastType, autohide: 2500 });
        }
        pollAndUpdate(true);
      } catch (err) {
        showToast('Error', 'Reserve failed', { type: 'error', autohide: 3500 });
      } finally {
        tdResv.removeAttribute('data-editing');
      }
    };
    actionsWrap.appendChild(reserveBtn);
  } else {
    // RELEASE button (for resv/static)
    const releaseBtn = ce('button', { class: 'btn btn-sm action-release', text: 'RELEASE', type: 'button' });
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
    actionsWrap.appendChild(releaseBtn);
  }

  // Only show EDIT button in preview mode (NOT delete)
  const editBtn = ce('button', {
    class: 'btn btn-sm btn-icon-only action-edit',
    text: '✏️',
    type: 'button',
    title: 'Edit Device'
  });

  editBtn.onclick = () => {
    if (editingRowId && editingRowId !== d.device_id) {
      showToast('Info', 'Finish editing current row first', { type: 'info', autohide: 1500 });
      return;
    }
    enableEditMode(tr, d);
  };
  actionsWrap.appendChild(editBtn);

  // DELETE button is now shown ONLY during edit mode (in enableEditMode function)

  tdActions.appendChild(actionsWrap);
}

/* =========================
Inline Add Row Functionality
========================= */
let inlineAddRow = null; // Track the inline add row element

function createInlineAddRow() {
  const tr = ce('tr', { id: 'inline-add-row', style: 'background: rgba(111,66,193,0.05);' });
  
  // Create input cells for each column (0-8), Actions cell (9) has Save/Cancel
  const fields = [
    { key: 'device_id', placeholder: 'Device ID', required: true },
    { key: 'model_name', placeholder: 'Model', required: true },
    { key: 'serial_no', placeholder: 'Serial No' },
    { key: 'hw_id', placeholder: 'Hardware ID' },
    { key: 'mgmt_ip', placeholder: 'Mgmt IP', required: true },
    { key: 'port_id', placeholder: 'Port', type: 'number', required: true },
    { key: 'tag', placeholder: 'free', value: 'free', hidden: true },
    { key: 'current_user', value: '', hidden: true },
    { key: 'duration', value: '', hidden: true },
    { key: 'resv_end_time', value: '', hidden: true }
  ];
  
  fields.forEach((field, idx) => {
    const td = ce('td', { class: 'col-left', style: 'padding: 0.3rem;' });
    if (field.hidden) {
      td.style.display = 'none';
    }
    const inp = ce('input', {
      type: field.type || 'text',
      class: 'inline-edit-input',
      placeholder: field.placeholder,
      value: field.value || '',
      'data-field': field.key,
      'data-required': field.required ? 'true' : 'false'
    });
    if (field.key === 'port_id') {
      inp.min = '0';
      inp.max = '999';
    }
    td.appendChild(inp);
    tr.appendChild(td);
  });
  
  // Actions cell (column 9)
  const tdActions = ce('td', { class: 'col-center', style: 'padding: 0.3rem;' });
  const saveBtn = ce('button', { class: 'btn btn-sm btn-success', text: '✓ Save', type: 'button' });
  const cancelBtn = ce('button', { class: 'btn btn-sm btn-outline-secondary ms-2', text: '✗ Cancel', type: 'button' });
  
  saveBtn.addEventListener('click', async () => {
    await submitInlineAddRow();
  });
  
  cancelBtn.addEventListener('click', () => {
    removeInlineAddRow();
  });
  
  tdActions.appendChild(saveBtn);
  tdActions.appendChild(cancelBtn);
  tr.appendChild(tdActions);
  
  return tr;
}

function showInlineAddRow() {
  if (inlineAddRow) return; // Already showing
  
  const tb = tbody();
  if (!tb) return;
  
  inlineAddRow = createInlineAddRow();
  tb.appendChild(inlineAddRow);
  
  // Focus first input
  const firstInput = inlineAddRow.querySelector('input');
  if (firstInput) firstInput.focus();
  
  showToast('Add Device', 'Fill in device details and click Save', { type: 'info', autohide: 2000 });
}

function removeInlineAddRow() {
  if (inlineAddRow) {
    inlineAddRow.remove();
    inlineAddRow = null;
  }
}

async function submitInlineAddRow() {
  if (!inlineAddRow) return;
  
  const inputs = inlineAddRow.querySelectorAll('input');
  const deviceData = {};
  
  // Validate required fields
  let valid = true;
  inputs.forEach(inp => {
    const field = inp.getAttribute('data-field');
    const required = inp.getAttribute('data-required') === 'true';
    const value = inp.value.trim();
    
    if (required && !value) {
      inp.style.borderColor = 'var(--status-resv)';
      valid = false;
    } else {
      inp.style.borderColor = '';
    }
    
    deviceData[field] = value;
  });
  
  if (!valid) {
    showToast('Error', 'Please fill all required fields', { type: 'error', autohide: 2500 });
    return;
  }
  
  // Validate IP format
  if (deviceData.mgmt_ip && !/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(deviceData.mgmt_ip)) {
    showToast('Error', 'Invalid IP address format', { type: 'error', autohide: 2500 });
    return;
  }
  
  try {
    const res = await postJSON(API.devicesAdd, deviceData);
    if (res && res.ok) {
      showToast('Success', `Device ${deviceData.device_id} added`, { type: 'success', autohide: 2000 });
      removeInlineAddRow();
      pollAndUpdate(true);
    } else {
      showToast('Error', res.error || 'Failed to add device', { type: 'error', autohide: 3000 });
    }
  } catch (err) {
    showToast('Error', 'Add device failed', { type: 'error', autohide: 3000 });
    console.error(err);
  }
}

/* =========================
Inline Edit Mode (Same Row - Not Separate Row)
========================= */
let editingRowId = null; // Track which row is being edited

function enableEditMode(tr, d) {
  if (editingRowId) return; // Only one row editable at a time
  editingRowId = d.device_id;
  
  const cells = tr.cells;
  
  // Store original values
  const originalValues = {
    device_id: d.device_id,
    model_name: d.model_name || '',
    serial_no: d.serial_no || '',
    hw_id: d.hw_id || '',
    mgmt_ip: d.mgmt_ip || '',
    port_id: d.port_id || ''
  };
  
  // 0: Device ID (now editable)
  if (cells[0]) {
    const idContent = cells[0].querySelector('div');
    if (idContent) {
      const idLines = idContent.querySelectorAll('div');
      if (idLines[0]) {
        const inp = ce('input', {
          type: 'text',
          class: 'inline-edit-input',
          value: d.device_id || '',
          'data-field': 'device_id',
          'data-original': d.device_id || ''
        });
        idLines[0].innerHTML = '';
        idLines[0].appendChild(inp);
      }
      if (idLines[1] && d.serial_no) {
        idLines[1].style.display = 'none'; // Hide serial_no during edit
      }
    }
  }
  
  // 1: Model Name
  if (cells[1]) {
    const inp = ce('input', {
      type: 'text',
      class: 'inline-edit-input',
      value: d.model_name || '',
      'data-field': 'model_name',
      'data-original': d.model_name || ''
    });
    cells[1].innerHTML = '';
    cells[1].appendChild(inp);
  }
  
  // 2: Serial No (FIXED: Now in correct column)
  if (cells[2]) {
    const inp = ce('input', {
      type: 'text',
      class: 'inline-edit-input',
      value: d.serial_no || '',
      'data-field': 'serial_no',
      'data-original': d.serial_no || ''
    });
    cells[2].innerHTML = '';
    cells[2].appendChild(inp);
  }
  
  // 3: Hardware ID
  if (cells[3]) {
    const inp = ce('input', {
      type: 'text',
      class: 'inline-edit-input',
      value: d.hw_id || '',
      'data-field': 'hw_id',
      'data-original': d.hw_id || ''
    });
    cells[3].innerHTML = '';
    cells[3].appendChild(inp);
  }
  
  // 4: Mgmt IP (allow blank, validate only if not empty)
  if (cells[4]) {
    const mgmtWrap = cells[4].querySelector('.mgmt-actions');
    if (mgmtWrap) {
      const mgmtText = mgmtWrap.querySelector('span');
      if (mgmtText) {
        const inp = ce('input', {
          type: 'text',
          class: 'inline-edit-input',
          value: d.mgmt_ip || '',
          'data-field': 'mgmt_ip',
          'data-original': d.mgmt_ip || '',
          placeholder: 'Leave blank for auto-discover'
        });
        mgmtText.style.display = 'none';
        mgmtWrap.insertBefore(inp, mgmtText);
      }
      // Hide icon buttons during edit
      mgmtWrap.querySelectorAll('.icon-only').forEach(btn => btn.style.display = 'none');
    }
  }
  
  // 5: Port ID
  if (cells[5]) {
    const consoleWrap = cells[5].querySelector('.console-actions');
    if (consoleWrap) {
      const portSpan = consoleWrap.querySelector('span');
      if (portSpan) {
        const inp = ce('input', {
          type: 'number',
          class: 'inline-edit-input',
          value: d.port_id || '',
          'data-field': 'port_id',
          'data-original': d.port_id || '',
          min: '0',
          max: '999'
        });
        portSpan.style.display = 'none';
        consoleWrap.insertBefore(inp, portSpan);
      }
      // Hide icon buttons during edit
      consoleWrap.querySelectorAll('.icon-only').forEach(btn => btn.style.display = 'none');
    }
  }
  
  // 9: Actions - Replace with Save/Cancel/Delete (LEFT ALIGNED)
  if (cells[9]) {
    const actionsWrap = cells[9].querySelector('.actions-wrap');
    if (actionsWrap) {
      actionsWrap.innerHTML = '';
      actionsWrap.style.justifyContent = 'flex-start'; // LEFT ALIGN
      
      const saveBtn = ce('button', {
        class: 'btn btn-sm btn-success',
        text: '✓ Save',
        type: 'button',
        title: 'Save changes'
      });
      
      const cancelBtn = ce('button', {
        class: 'btn btn-sm btn-outline-secondary',
        text: '✗ Cancel',
        type: 'button',
        title: 'Cancel editing'
      });
      
      const deleteBtn = ce('button', {
        class: 'btn btn-sm btn-outline-danger',
        text: '🗑️ Delete',
        type: 'button',
        title: 'Delete device'
      });
      
      saveBtn.addEventListener('click', async () => {
        await submitInlineEdit(tr, d, originalValues);
      });
      
      cancelBtn.addEventListener('click', () => {
        disableEditMode(tr, d);
      });
      
      deleteBtn.addEventListener('click', async () => {
        const confirmed = confirm(`Are you sure you want to delete device ${d.device_id}? This cannot be undone.`);
        if (!confirmed) return;
        try {
          const r = await postJSON(API.devicesDelete, { device_id: d.device_id });
          if (r && r.ok) {
            showToast('Delete', `Device ${d.device_id} deleted`, { type: 'success', autohide: 2000 });
            editingRowId = null;
            pollAndUpdate(true);
          } else {
            showToast('Error', r.error || 'Delete failed', { type: 'error', autohide: 3000 });
          }
        } catch (err) {
          showToast('Error', 'Delete failed', { type: 'error', autohide: 3000 });
          console.error(err);
        }
      });
      
      actionsWrap.appendChild(saveBtn);
      actionsWrap.appendChild(cancelBtn);
      actionsWrap.appendChild(deleteBtn);
    }
  }
  
  // Focus first input
  const firstInput = tr.querySelector('.inline-edit-input');
  if (firstInput) firstInput.focus();
  
  showToast('Edit', `Editing ${d.device_id}`, { type: 'info', autohide: 1500 });
}

function disableEditMode(tr, d) {
  editingRowId = null;
  setupReservationAndActions(tr, d); // Re-render reservation/actions
  pollAndUpdate(true); // Refresh to restore original values
}

async function submitInlineEdit(tr, d, originalValues) {
  const inputs = tr.querySelectorAll('.inline-edit-input');
  const deviceData = { device_id: originalValues.device_id }; // Use original device_id for lookup
  let hasChanges = false;
  let valid = true;
  
  inputs.forEach(inp => {
    const field = inp.getAttribute('data-field');
    const original = inp.getAttribute('data-original');
    const value = inp.value.trim();
    
    // Validate IP only if not empty
    if (field === 'mgmt_ip' && value && !/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}$/.test(value)) {
      inp.style.borderColor = 'var(--status-resv)';
      showToast('Error', 'Invalid IP address format', { type: 'error', autohide: 2500 });
      valid = false;
    } else {
      inp.style.borderColor = '';
    }
    
    if (value !== original) {
      hasChanges = true;
    }
    
    deviceData[field] = value;
  });
  
  if (!valid) return;
  
  if (!hasChanges) {
    showToast('Info', 'No changes made', { type: 'info', autohide: 1500 });
    disableEditMode(tr, d);
    return;
  }
  
  try {
    const res = await postJSON(API.devicesEdit, deviceData);
    if (res && res.ok) {
      showToast('Success', `Device ${deviceData.device_id} updated`, { type: 'success', autohide: 2000 });
      editingRowId = null;
      pollAndUpdate(true);
    } else {
      showToast('Error', res.error || 'Failed to update device', { type: 'error', autohide: 3000 });
    }
  } catch (err) {
    showToast('Error', 'Update failed', { type: 'error', autohide: 3000 });
    console.error(err);
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

  // 0: id (device_id + serial_no)
  if (cells[0]) {
    const idContent = cells[0].querySelector('div');
    if (idContent) {
      const idLines = idContent.querySelectorAll('div');
      if (idLines[0]) idLines[0].innerText = String(d.device_id || '');
      if (idLines[1]) {
        idLines[1].innerText = String(d.serial_no || '');
        idLines[1].style.display = d.serial_no ? 'block' : 'none';
      }
    }
  }

  // 1: model
  if (cells[1] && cells[1].innerText !== (d.model_name || '')) cells[1].innerText = d.model_name || '';

  // 2: serial_no (NEW - was missing)
  if (cells[2] && cells[2].innerText !== (d.serial_no || '')) cells[2].innerText = d.serial_no || '';

  // 3: hardware
  if (cells[3] && cells[3].innerText !== (d.hw_id || '')) cells[3].innerText = d.hw_id || '';


  // 5: mgmt ip (span)
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

  // 6: console port
  if (cells[5]) {
    const portSpan = cells[5].querySelector('span');
    const newPort = (d.port_id == null || d.port_id === '') ? '-' : String(d.port_id);
    if (portSpan && portSpan.innerText !== newPort) portSpan.innerText = newPort;
  }

  // 7: health
  if (cells[6]) {
    const hspan = cells[6].querySelector('span');
    const newHealth = (d.health || 'unk');
    if (hspan && hspan.innerText !== newHealth) {
      hspan.innerText = newHealth;
      hspan.className = mapHealthClass(newHealth);
    }
  }

  // 8: status
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

  // 9 & 10: reservation + actions - re-setup to reflect changed tag/user
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
    const resp = await getJSON(API.devicesView);
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

  const addBtn = document.getElementById('addDeviceBtn');
  if (addBtn) {
    addBtn.addEventListener('click', () => {
      // Close any existing edit row first
      if (inlineEditRow) removeInlineEditRow();
      showInlineAddRow();
    });
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