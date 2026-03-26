/* =========================
 * 1. Configuration
 * ========================= */
const API = {
    devices: '/api/devices',
    config: '/api/config',
    removeMgmtIp: '/api/remove_mgmt_ip',
    refreshHealth: '/api/refresh_health',
    reserve: '/api/reserve',
    release: '/api/release',
    dbAdd: '/api/db/add',
    dbEdit: '/api/db/edit',
    dbDel: '/api/db/delete',
    dbConsoleIp: '/api/db/console/ip',
    dbConsolePort: '/api/db/console/port',
};

const POLL_INTERVAL_MS = 1000;
const UPDATE_DEBOUNCE_MS = 100;
let PORT_OFFSET = 10000;
const ROW_ID_PREFIX = 'row-';
const UPDATABLE_FIELDS = ["device_id", "serial_no", "model_name", "hw_id", "console_ip", "port_id"];
const FILTER_DEFS = {
    model: { inputId: 'filterModelInput', toggleId: 'filterModelToggle', clearId: 'clearFilterModel', listId: 'filterModelList' },
    hardware: { inputId: 'filterHardwareInput', toggleId: 'filterHardwareToggle', clearId: 'clearFilterHardware', listId: 'filterHardwareList' }
};
const filterState = { model: '', hardware: '' };
let filterEls = null;
const filterOptionsCache = { model: [], hardware: [] };

/* =========================
 * 2. Utility Helpers
 * ========================= */
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
    try { return JSON.parse(text); } catch (e) { return text ? { text } : {}; }
}

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
 * 3. Toast Notifications
 * ========================= */
const TOAST_CONTAINER_ID = 'app-toast-container';

function ensureToastContainer() {
    let c = document.getElementById(TOAST_CONTAINER_ID);
    if (c) return c;
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
    const type = opts.type || 'info';
    if (type === 'success') body.style.backgroundColor = 'rgba(40,167,69,0.12)';
    if (type === 'error') body.style.backgroundColor = 'rgba(220,53,69,0.12)';
    let bsToast = null;
    const autohide = opts.autohide !== false;
    const delay = typeof opts.autohide === 'number' ? opts.autohide : 3500;
    try {
        bsToast = new bootstrap.Toast(toastEl, { autohide: autohide, delay: delay });
        bsToast.show();
        toastEl.addEventListener('hidden.bs.toast', () => toastEl.remove());
    } catch (e) {
        setTimeout(() => toastEl.remove(), delay);
    }
}

/* =========================
 * 4. Button Styling Helper
 * ========================= */
function bootstrapifyButtons(root) {
    if (!root) return;
    root.querySelectorAll('.icon-only').forEach(btn => {
        if (!btn.classList.contains('btn')) btn.classList.add('btn', 'btn-outline-secondary', 'btn-sm');
    });
    root.querySelectorAll('.action-reserve').forEach(b => {
        if (!b.classList.contains('btn')) b.classList.add('btn', 'btn-sm', 'btn-primary');
    });
    root.querySelectorAll('.action-release').forEach(b => {
        if (!b.classList.contains('btn')) b.classList.add('btn', 'btn-sm', 'btn-outline-secondary');
    });
    root.querySelectorAll('.action-delete').forEach(b => {
        if (!b.classList.contains('btn')) b.classList.add('btn', 'btn-sm', 'btn-outline-danger');
    });
}

/* =========================
 * 5A. Table Theme Toggle
 * ========================= */
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
    console.info('Theme switched to', next);
}

(function initTheme() {
    try {
        const saved = localStorage.getItem(THEME_KEY) || 'light';
        setTheme(saved);
        const btn = document.getElementById('themeToggle');
        if (btn) btn.addEventListener('click', toggleTheme);
    } catch (e) {}
})();

/* =========================
 * 5B. Table Filters
 * ========================= */
function normalizeFilterValue(val) {
    return String(val || '').trim().toLowerCase();
}

function getFilterElements() {
    if (filterEls) return filterEls;
    filterEls = {
        modelInput: document.getElementById(FILTER_DEFS.model.inputId),
        modelToggle: document.getElementById(FILTER_DEFS.model.toggleId),
        modelClear: document.getElementById(FILTER_DEFS.model.clearId),
        modelList: document.getElementById(FILTER_DEFS.model.listId),
        hardwareInput: document.getElementById(FILTER_DEFS.hardware.inputId),
        hardwareToggle: document.getElementById(FILTER_DEFS.hardware.toggleId),
        hardwareClear: document.getElementById(FILTER_DEFS.hardware.clearId),
        hardwareList: document.getElementById(FILTER_DEFS.hardware.listId)
    };
    return filterEls;
}

function refreshFilterStateFromUi() {
    const els = getFilterElements();
    if (!els.modelInput || !els.hardwareInput) return;
    filterState.model = normalizeFilterValue(els.modelInput.value);
    filterState.hardware = normalizeFilterValue(els.hardwareInput.value);
    updateFilterClearState();
}

function updateFilterClearState() {
    const els = getFilterElements();
    if (els.modelClear) els.modelClear.disabled = !filterState.model;
    if (els.hardwareClear) els.hardwareClear.disabled = !filterState.hardware;
}

function closeFilterDropdown(kind) {
    const els = getFilterElements();
    if (kind === 'model') {
        if (els.modelList) els.modelList.hidden = true;
        if (els.modelToggle) els.modelToggle.setAttribute('aria-expanded', 'false');
    } else {
        if (els.hardwareList) els.hardwareList.hidden = true;
        if (els.hardwareToggle) els.hardwareToggle.setAttribute('aria-expanded', 'false');
    }
}

function closeAllFilterDropdowns() {
    closeFilterDropdown('model');
    closeFilterDropdown('hardware');
}

function populateFilterDropdownList(kind) {
    const els = getFilterElements();
    const ul = kind === 'model' ? els.modelList : els.hardwareList;
    const input = kind === 'model' ? els.modelInput : els.hardwareInput;
    if (!ul) return;
    const all = filterOptionsCache[kind] || [];
    const q = normalizeFilterValue(input ? input.value : '');
    const filtered = q
        ? all.filter(v => normalizeFilterValue(v).includes(q))
        : all.slice();
    ul.innerHTML = '';
    filtered.forEach(val => {
        const li = ce('li', { role: 'option', text: val });
        li.addEventListener('mousedown', (e) => { e.preventDefault(); });
        li.addEventListener('click', () => selectFilterOption(kind, val));
        ul.appendChild(li);
    });
}

function selectFilterOption(kind, val) {
    const els = getFilterElements();
    const input = kind === 'model' ? els.modelInput : els.hardwareInput;
    if (input) input.value = val;
    closeFilterDropdown(kind);
    refreshFilterStateFromUi();
    applyFilters();
}

function toggleFilterDropdown(kind) {
    const els = getFilterElements();
    const list = kind === 'model' ? els.modelList : els.hardwareList;
    const toggle = kind === 'model' ? els.modelToggle : els.hardwareToggle;
    const other = kind === 'model' ? 'hardware' : 'model';
    if (!list || !toggle) return;
    const isOpen = !list.hidden;
    if (isOpen) {
        closeFilterDropdown(kind);
        return;
    }
    closeFilterDropdown(other);
    populateFilterDropdownList(kind);
    list.hidden = false;
    toggle.setAttribute('aria-expanded', 'true');
}

function commonPrefixInsensitive(values) {
    if (!values || values.length === 0) return '';
    let prefix = values[0];
    for (let i = 1; i < values.length; i++) {
        const s = values[i];
        let j = 0;
        const max = Math.min(prefix.length, s.length);
        while (j < max && prefix[j].toLowerCase() === s[j].toLowerCase()) j++;
        prefix = prefix.slice(0, j);
        if (!prefix) break;
    }
    return prefix;
}

function tryAutoFillInput(kind, input, ev) {
    if (!input) return;
    const raw = input.value || '';
    if (!raw) return;
    if (input.selectionStart == null || input.selectionEnd == null) return;
    const caretAtEnd = input.selectionStart === input.selectionEnd && input.selectionEnd === raw.length;
    if (!caretAtEnd) return;
    if (ev && ev.inputType && ev.inputType.startsWith('delete')) return;
    const options = filterOptionsCache[kind] || [];
    if (options.length === 0) return;
    const matches = options.filter(v => normalizeFilterValue(v).includes(normalizeFilterValue(raw)));
    const prefix = commonPrefixInsensitive(matches);
    if (!prefix) return;
    const rawNorm = normalizeFilterValue(raw);
    const prefixNorm = normalizeFilterValue(prefix);
    if (!prefixNorm.startsWith(rawNorm)) return;
    if (prefix.length <= raw.length) return;
    input.value = prefix;
    input.setSelectionRange(raw.length, prefix.length);
}

function syncFilterOptions(devices) {
    const els = getFilterElements();
    if (!els.modelInput || !els.hardwareInput) return;
    const modelVals = new Set();
    const hwVals = new Set();
    (devices || []).forEach(d => {
        const model = String(d.model_name || '').trim();
        const hw = String(d.hw_id || '').trim();
        if (model) modelVals.add(model);
        if (hw) hwVals.add(hw);
    });
    filterOptionsCache.model = Array.from(modelVals).sort((a, b) => a.localeCompare(b, undefined, { sensitivity: 'base' }));
    filterOptionsCache.hardware = Array.from(hwVals).sort((a, b) => a.localeCompare(b, undefined, { sensitivity: 'base' }));
    if (els.modelList && !els.modelList.hidden) populateFilterDropdownList('model');
    if (els.hardwareList && !els.hardwareList.hidden) populateFilterDropdownList('hardware');
    refreshFilterStateFromUi();
}

function applyFilters() {
    const modelQ = filterState.model;
    const hwQ = filterState.hardware;
    const tb = tbody();
    if (!tb) return;
    tb.querySelectorAll('tr').forEach(row => {
        if (!row.id || !row.id.startsWith(ROW_ID_PREFIX)) return;
        const rowModel = normalizeFilterValue(row.cells[1] ? row.cells[1].innerText : '');
        const rowHw = normalizeFilterValue(row.cells[2] ? row.cells[2].innerText : '');
        const matchesModel = !modelQ || rowModel.includes(modelQ);
        const matchesHw = !hwQ || rowHw.includes(hwQ);
        row.style.display = (matchesModel && matchesHw) ? '' : 'none';
    });
}

function onFilterTextInput(kind, ev) {
    const els = getFilterElements();
    const input = kind === 'model' ? els.modelInput : els.hardwareInput;
    const list = kind === 'model' ? els.modelList : els.hardwareList;
    refreshFilterStateFromUi();
    applyFilters();
    const before = input ? input.value : '';
    tryAutoFillInput(kind, input, ev);
    if (input && input.value !== before) {
        refreshFilterStateFromUi();
        applyFilters();
    }
    if (list && !list.hidden) populateFilterDropdownList(kind);
}

function initFilters() {
    const els = getFilterElements();
    if (!els.modelInput || !els.hardwareInput) return;
    document.addEventListener('click', (e) => {
        if (e.target.closest('.filter-combo')) return;
        closeAllFilterDropdowns();
    });
    if (els.modelToggle) {
        els.modelToggle.addEventListener('click', (e) => {
            e.stopPropagation();
            toggleFilterDropdown('model');
        });
    }
    if (els.hardwareToggle) {
        els.hardwareToggle.addEventListener('click', (e) => {
            e.stopPropagation();
            toggleFilterDropdown('hardware');
        });
    }
    els.modelInput.addEventListener('input', (ev) => onFilterTextInput('model', ev));
    els.hardwareInput.addEventListener('input', (ev) => onFilterTextInput('hardware', ev));
    function onFilterKeydown(kind, e) {
        if (e.key === 'Escape') {
            e.stopPropagation();
            closeFilterDropdown(kind);
        }
    }
    els.modelInput.addEventListener('keydown', (e) => onFilterKeydown('model', e));
    els.hardwareInput.addEventListener('keydown', (e) => onFilterKeydown('hardware', e));
    if (els.modelClear) {
        els.modelClear.addEventListener('click', (e) => {
            e.stopPropagation();
            els.modelInput.value = '';
            closeFilterDropdown('model');
            refreshFilterStateFromUi();
            applyFilters();
        });
    }
    if (els.hardwareClear) {
        els.hardwareClear.addEventListener('click', (e) => {
            e.stopPropagation();
            els.hardwareInput.value = '';
            closeFilterDropdown('hardware');
            refreshFilterStateFromUi();
            applyFilters();
        });
    }
    refreshFilterStateFromUi();
}

/* =========================
 * 6. Core Update Logic
 * ========================= */
let prevDevices = new Map();
let updateTimer = null;
let isUpdating = false;
let pendingPollForce = false; // If true, run another full poll after the current one

function deviceKey(d) {
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

function tbody() {
    return document.querySelector('#devices_table tbody');
}

/** Focus is on an interactive control in this row — avoid rebuilding reservation/actions DOM (stops caret jumping on poll). */
function rowHasInteractiveFocus(tr) {
    const ae = document.activeElement;
    if (!ae || !tr.contains(ae)) return false;
    const t = ae.tagName;
    return t === 'INPUT' || t === 'TEXTAREA' || t === 'SELECT' || t === 'BUTTON';
}

/* =========================
 * 7. Create Row for Device
 * ========================= */
function createRowForDevice(d) {
    const safe = (v, fallback = '') => (v === null || v === undefined) ? fallback : v;
    const tr = ce('tr', { id: ROW_ID_PREFIX + d.device_id });
    
    // 0: Id (device_id + serial_no)
    const tdId = ce('td', { class: 'col-left word-wrap' });
    const idContent = ce('div', { style: 'display: flex; flex-direction: column; line-height: 1.3;' });
    idContent.appendChild(ce('div', { style: 'font-weight: 600; font-size: 0.95rem;', text: String(safe(d.device_id)) }));
    if (d.serial_no) {
        idContent.appendChild(ce('div', { style: 'font-size: 0.78rem; color: var(--muted); margin-top: 0.15rem;', text: String(d.serial_no) }));
    }
    tdId.appendChild(idContent);
    tr.appendChild(tdId);
    
    // 1: Model
    tr.appendChild(ce('td', { class: 'col-left word-wrap', text: safe(d.model_name, '') }));
    
    // 2: Hardware
    tr.appendChild(ce('td', { class: 'col-left', text: safe(d.hw_id, '') }));
    
    // 3: UI (stack of links)
    const tdUI = ce('td', { class: 'col-center' }, ce('div', { class: 'ui-stack', text: '— — —' }));
    tr.appendChild(tdUI);
    
    // 4: Mgmt-Ip
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
                showToast('Mgmt IP', 'Mgmt-IP removed. Refresh health to fetch new IP', { type: 'info', autohide: 1400 });
                pollAndUpdate(true);
            } catch (err) {
                showToast('Error', 'Failed to remove mgmt-ip', { type: 'error' });
                console.error(err);
            }
        });
        const ssh = `ssh admin@${d.mgmt_ip}`;
        const copyBtn = ce('button', { type: 'button', class: 'icon-only', title: ssh }, '⧉');
        copyBtn.addEventListener('click', () => {
            copyToClipboard(ssh);
            showToast('Copied', ssh, { autohide: 1200, type: 'info' });
        });
        mgmtWrap.appendChild(removeBtn);
        mgmtWrap.appendChild(copyBtn);
    }
    tdMgmt.appendChild(mgmtWrap);
    tr.appendChild(tdMgmt);
    
    // 5: Console IP (line 1) + port + copy (line 2)
    const tdConsole = ce('td', { class: 'col-left' });
    const consoleWrap = ce('div', { class: 'console-cell' });
    const ipLine = ce('div', { class: 'console-ip-line' });
    const ipInp = ce('input', {
        type: 'text',
        class: 'console-ip-input',
        value: d.console_ip || '',
        placeholder: '—.—.—.—',
        inputmode: 'decimal',
        autocomplete: 'on'
    });
    ipInp.addEventListener('change', async () => {
        const val = ipInp.value.trim();
        try {
            const res = await postJSON(API.dbConsoleIp, { device_id: d.device_id, console_ip: val });
            if (res.ok) {
                showToast('Console IP', val ? `Set to ${val}` : 'Console IP updated', { type: 'success', autohide: 1500 });
                if (res.mgmt_ip) {
                    showToast('Auto-Discover', `mgmt_ip discovered: ${res.mgmt_ip}`, { type: 'success', autohide: 2000 });
                }
                pollAndUpdate(true);
            } else {
                showToast('Error', res.error || res.reason || 'Failed to update console IP', { type: 'error' });
                ipInp.value = d.console_ip || '';
            }
        } catch (err) {
            showToast('Error', 'Failed to update console IP', { type: 'error' });
            ipInp.value = d.console_ip || '';
        }
    });
    ipLine.appendChild(ipInp);
    consoleWrap.appendChild(ipLine);

    const portRow = ce('div', { class: 'console-port-row' });
    const portInp = ce('input', {
        type: 'number',
        class: 'console-port-input',
        value: (d.port_id == null || d.port_id === '') ? '' : String(d.port_id),
        min: 1,
        max: 64,
        style: 'width: 4rem; text-align: center;'
    });
    portInp.addEventListener('change', async () => {
        let val = parseInt(portInp.value);
        if (val < 1) { portInp.value = 1; val = 1; }
        if (val > 64) { portInp.value = 64; val = 64; }
        try {
            const res = await postJSON(API.dbConsolePort, { device_id: d.device_id, port_id: val });
            if (res.ok) {
                showToast('Port Updated', `Console port set to ${val}`, { type: 'success', autohide: 1500 });
                if (res.mgmt_ip) {
                    showToast('Auto-Discover', `mgmt_ip discovered: ${res.mgmt_ip}`, { type: 'success', autohide: 2000 });
                }
                pollAndUpdate(true);
            } else {
                showToast('Error', res.error || 'Failed to update port', { type: 'error' });
                portInp.value = d.port_id || '';
            }
        } catch (err) {
            showToast('Error', 'Failed to update port', { type: 'error' });
            portInp.value = d.port_id || '';
        }
    });
    portRow.appendChild(portInp);

    if (d.console_ip && d.port_id) {
        const p0 = parseInt(String(d.port_id), 0);
        const telnet = `telnet ${d.console_ip} ${PORT_OFFSET + p0}`;
        const copyBtn = ce('button', { type: 'button', class: 'icon-only console-copy-btn', title: telnet }, '⧉');
        copyBtn.addEventListener('click', () => {
            copyToClipboard(telnet);
            showToast('Copied', telnet, { autohide: 1200, type: 'info' });
        });
        portRow.appendChild(copyBtn);
    }
    consoleWrap.appendChild(portRow);
    tdConsole.appendChild(consoleWrap);
    tr.appendChild(tdConsole);
    
    // 6: Health
    const tdHealth = ce('td', { class: 'col-left' });
    const healthWrap = ce('div', { class: 'cell-flex' });
    const healthSpan = ce('span', { text: safe(d.health, 'unk') });
    healthSpan.className = mapHealthClass(d.health);
    healthWrap.appendChild(healthSpan);
    const healthRefresh = ce('button', { type: 'button', class: 'icon-only', title: 'Refresh' }, '↻');
    healthRefresh.addEventListener('click', async () => {
        try {
            healthSpan.innerText = '...';
            const j = await postJSON(API.refreshHealth, { device_id: d.device_id });
            const newStatus = (j.status || j.state || 'unk').toLowerCase();
            healthSpan.innerText = newStatus;
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
    
    // 7: Status
    const tdStatus = ce('td', { class: 'col-center' });
    const statusSpan = ce('span', {});
    setStatusSpanForTag(statusSpan, d.tag);
    tdStatus.appendChild(statusSpan);
    tr.appendChild(tdStatus);
    
    // 8: Reservation
    const tdResv = ce('td', { class: 'col-left' });
    tdResv.appendChild(ce('div', { class: 'resv-container' }));
    tr.appendChild(tdResv);
    
    // 9: Actions (RESERVE/RELEASE + DELETE)
    const tdActions = ce('td', { class: 'col-center' });
    tdActions.appendChild(ce('div', { class: 'actions-wrap' }));
    tr.appendChild(tdActions);
    
    bootstrapifyButtons(tr);
    
    // Populate UI config asynchronously
    (async function populateConfig(mgmt_ip, port_id) {
        if (!mgmt_ip || mgmt_ip === '-' || mgmt_ip === '—.—.—.—') return;
        try {
            const cfg = await postJSON(API.config, {mgmt_ip});
            if (!cfg) return;
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
            const po = cfg.port_offset;
            if (po != null && po !== '' && !Number.isNaN(Number(po)))
                PORT_OFFSET = Number(po);
        } catch (err) {
            console.debug('config fetch failed for', d.device_id, err);
        }
    })(d.mgmt_ip, d.port_id);
    
    setupReservationAndActions(tr, d);
    return tr;
}

/* =========================
 * 8. Health Class Mapping
 * ========================= */
function mapHealthClass(h) {
    if (!h) return 'health-unk';
    const s = String(h).toLowerCase();
    if (s.includes('up')) return 'health-up';
    if (s.includes('down') || s.includes('fail')) return 'health-down';
    if (s.includes('busy') || s.includes('loading')) return 'health-busy';
    return 'health-unk';
}

function setStatusSpanForTag(span, tag) {
    const t = (tag || 'free').toLowerCase();
    span.className = t === 'free' ? 'status-free' : (t === 'resv' ? 'status-resv' : (t === 'static' ? 'status-static' : ''));
    span.innerText = t === 'free' ? 'Free' : (t === 'resv' ? 'Reserved' : (t === 'static' ? 'Static' : String(tag || '')));
}

/** data-editing can stick on <td> while innerHTML shows reserved <pre>; only block updates when the free-row form exists or an input here is focused. */
function reservationCellIsActivelyEditing(tdResv) {
    if (!tdResv) return false;
    if (tdResv.querySelector('input:focus')) return true;
    if (tdResv.getAttribute('data-editing') !== '1') return false;
    return !!tdResv.querySelector('.resv-container');
}

/* =========================
 * 9. Reservation & Actions Setup
 * ========================= */
function setupReservationAndActions(tr, d) {
    const cells = tr.cells;
    const tdResv = cells[8];
    const tdActions = cells[9];
    if (!tdResv || !tdActions) return;
    if (reservationCellIsActivelyEditing(tdResv)) return;
    tdResv.innerHTML = '';
    tdActions.innerHTML = '';
    const tag = String(d.tag || 'free').trim().toLowerCase();
    const actionsWrap = tdActions.querySelector('.actions-wrap') || tdActions;
    actionsWrap.innerHTML = '';
    
    if (tag === 'free') {
        // Reservation inputs
        const container = ce('div', { class: 'resv-container' });
        const inputsDiv = ce('div', { class: 'resv-inputs' });
        const userInp = ce('input', { type: 'text', class: 'input-user', placeholder: 'Username', value: d.current_user || '' });
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
        [userInp, hrsInp, minsInp].forEach(el => {
            el.addEventListener('focus', () => tdResv.setAttribute('data-editing', '1'));
            el.addEventListener('blur', () => setTimeout(() => tdResv.removeAttribute('data-editing'), 200));
        });
        
        // Reserve button
        const reserveBtn = ce('button', { class: 'btn btn-sm action-reserve', text: 'RESERVE' });
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
                    showToast('Reserve', `Reservation: ${r.msg || '?'}`, { type: toastType, autohide: 2500 });
                }
            } catch (err) {
                showToast('Error', 'Reserve failed', { type: 'error', autohide: 3500 });
            } finally {
                tdResv.removeAttribute('data-editing');
            }
            // Refresh after clearing data-editing.
            pollAndUpdate(true).catch(e => console.error(e));
        };
        actionsWrap.appendChild(reserveBtn);
    } else {
        // Reservation info
        const pre = ce('pre', {
            style: 'margin: 0; font-family: monospace; text-align: left;',
            text: d.resv_block || (tag === 'static' ? 'Static assignment' : '')
        });
        tdResv.appendChild(pre);
        
        // Release button
        const releaseBtn = ce('button', { class: 'btn btn-sm action-release', text: 'RELEASE' });
        releaseBtn.onclick = async () => {
            const deviceId = d.device_id;
            try {
                const r = await postJSON(API.release, { device_id: deviceId });
                if (r && (r.ok || r.status === 'success')) {
                    showToast('Release', 'Device released successfully', { type: 'success', autohide: 2500 });
                }
            } catch (err) {
                showToast('Error', 'Release failed', { type: 'error', autohide: 3500 });
                return;
            }
            // Drop focus so updateRow does not skip the free-state rebuild (focus often stays on RELEASE).
            try {
                if (document.activeElement && document.body.contains(document.activeElement)) {
                    document.activeElement.blur();
                }
            } catch (e) { /* ignore */ }
            try {
                const devices = await fetchDevices();
                const fresh = devices.find(x => String(x.device_id) === String(deviceId));
                if (fresh) updateRow(fresh);
            } catch (e) {
                console.error(e);
            }
            pollAndUpdate(true).catch(e => console.error(e));
        };
        actionsWrap.appendChild(releaseBtn);
    }
    
    // Delete button (always shown)
    const deleteBtn = ce('button', {
        class: 'btn btn-sm action-delete',
        text: '🗑',
        title: 'Delete device from database'
    });
    deleteBtn.onclick = async () => {
        if (!confirm(`Are you sure you want to delete device ${d.device_id}? This cannot be undone.`)) return;
        try {
            const r = await postJSON(API.dbDel, { device_id: d.device_id });
            if (r && r.ok) {
                showToast('Deleted', `Device ${d.device_id} removed`, { type: 'success', autohide: 2500 });
                pollAndUpdate(true);
            } else {
                showToast('Error', r.error || 'Delete failed', { type: 'error', autohide: 3500 });
            }
        } catch (err) {
            showToast('Error', 'Delete failed', { type: 'error', autohide: 3500 });
        }
    };
    actionsWrap.appendChild(deleteBtn);
    bootstrapifyButtons(actionsWrap);
}

/* =========================
 * 10. Update Row (Minimal)
 * ========================= */
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
    
    // 2: hardware
    if (cells[2] && cells[2].innerText !== (d.hw_id || '')) cells[2].innerText = d.hw_id || '';
    
    // 4: mgmt ip
    if (cells[4]) {
        const mgmtSpan = cells[4].querySelector('span');
        const newMgmt = d.mgmt_ip || '—.—.—.—';
        if (mgmtSpan && mgmtSpan.innerText !== newMgmt) mgmtSpan.innerText = newMgmt;
        const hasMgmtBtn = !!cells[4].querySelector('.icon-only');
        if (d.mgmt_ip && !hasMgmtBtn) {
            const mgmtWrap = cells[4].querySelector('.mgmt-actions') || cells[4].querySelector('.cell-flex');
            if (mgmtWrap) {
                mgmtWrap.querySelectorAll('.icon-only').forEach(n => n.remove());
                const removeBtn = ce('button', { type: 'button', class: 'icon-only', title: 'Remove Mgmt-IP' }, '✖');
                removeBtn.addEventListener('click', async (ev) => {
                    ev.preventDefault();
                    try {
                        await postJSON(API.removeMgmtIp, { device_id: d.device_id });
                        pollAndUpdate(true);
                    } catch (err) {
                        showToast('Error', 'Failed to remove mgmt-ip', { type: 'error' });
                    }
                });
                const ssh = `ssh admin@${d.mgmt_ip}`;
                const copyBtn = ce('button', { type: 'button', class: 'icon-only', title: ssh }, '⧉');
                copyBtn.addEventListener('click', () => {
                    copyToClipboard(ssh);
                    showToast('Copied', ssh);
                });
                mgmtWrap.appendChild(removeBtn);
                mgmtWrap.appendChild(copyBtn);
                bootstrapifyButtons(mgmtWrap);
            }
        }
    }
    
    // 5: console IP + port inputs; copy btn on row 2 when IP & port exist
    if (cells[5]) {
        const ipInp = cells[5].querySelector('.console-ip-input');
        const newIp = d.console_ip || '';
        if (ipInp && document.activeElement !== ipInp && ipInp.value !== newIp) ipInp.value = newIp;

        const portInp = cells[5].querySelector('.console-port-input');
        const newPort = (d.port_id == null || d.port_id === '') ? '' : String(d.port_id);
        if (portInp && document.activeElement !== portInp && portInp.value !== newPort) portInp.value = newPort;

        const portRow = cells[5].querySelector('.console-port-row');
        let copyBtn = cells[5].querySelector('.console-copy-btn');
        const portNum = parseInt(String(d.port_id || ''), 0);
        const showCopy = !!(d.console_ip && d.port_id && !Number.isNaN(portNum) && portNum > 0);
        const telnet = showCopy ? `telnet ${d.console_ip} ${PORT_OFFSET + portNum}` : '';
        if (showCopy) {
            if (!copyBtn && portRow) {
                copyBtn = ce('button', { type: 'button', class: 'icon-only console-copy-btn', title: telnet }, '⧉');
                copyBtn.addEventListener('click', () => {
                    copyToClipboard(telnet);
                    showToast('Copied', telnet, { autohide: 1200, type: 'info' });
                });
                portRow.appendChild(copyBtn);
                bootstrapifyButtons(portRow);
            } else if (copyBtn) copyBtn.title = telnet;
        } else if (copyBtn) copyBtn.remove();
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
    
    // 8 & 9: reservation + actions
    const tdResv = cells[8];
    const tdActions = cells[9];
    if (!tdResv || !tdActions) return;
    if (!reservationCellIsActivelyEditing(tdResv)) {
        const srvTag = String(d.tag || 'free').trim().toLowerCase();
        const domFreeForm = !!tdResv.querySelector('.resv-container');
        const domShowsReservedChrome = !!(tdResv.querySelector('pre')) || !!tr.querySelector('.action-release');
        const serverSaysFree = srvTag === 'free';
        const structureMismatch =
            (serverSaysFree && domShowsReservedChrome) || (!serverSaysFree && domFreeForm);

        if (structureMismatch) {
            setupReservationAndActions(tr, d);
        } else if (rowHasInteractiveFocus(tr)) {
            const pre = tdResv.querySelector('pre');
            if ((srvTag === 'resv' || srvTag === 'static') && pre) {
                pre.textContent = d.resv_block || (srvTag === 'static' ? 'Static assignment' : '');
            }
        } else {
            setupReservationAndActions(tr, d);
        }
    }
}

/* =========================
 * 11. Polling & Updates
 * ========================= */
async function fetchDevices() {
    try {
        const resp = await getJSON(API.devices);
        if (Array.isArray(resp)) return resp;
        if (resp && Array.isArray(resp.devices)) return resp.devices;
        console.warn('fetchDevices: unexpected response shape', resp);
        return [];
    } catch (err) {
        console.error('fetchDevices error', err);
        return [];
    }
}

function scheduleUpdate(force = false) {
    if (updateTimer) clearTimeout(updateTimer);
    updateTimer = setTimeout(() => pollAndUpdate(force).catch(e => console.error(e)), UPDATE_DEBOUNCE_MS);
}

async function pollAndUpdate(force = false) {
    if (isUpdating) {
        if (force) pendingPollForce = true;
        return;
    }
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
        Array.from(tb.querySelectorAll('tr[id^="' + ROW_ID_PREFIX + '"]')).forEach(r => {
            const rid = r.id.replace(ROW_ID_PREFIX, '');
            if (!keep.has(rid)) {
                r.remove();
                prevDevices.delete(rid);
            }
        });
        syncFilterOptions(devices);
        applyFilters();
    } finally {
        isUpdating = false;
        if (pendingPollForce) {
            pendingPollForce = false;
            Promise.resolve().then(() => pollAndUpdate(true).catch(e => console.error(e)));
        }
    }
}

let pollIntervalHandle = null;
function startPolling() {
    pollAndUpdate(true);
    if (pollIntervalHandle) clearInterval(pollIntervalHandle);
    pollIntervalHandle = setInterval(() => scheduleUpdate(false), POLL_INTERVAL_MS);
}

function stopPolling() {
    if (pollIntervalHandle) clearInterval(pollIntervalHandle);
    pollIntervalHandle = null;
}

/* =========================
 * 12. Inline Add Form (Bottom Bar)
 * ========================= */
let addFormVisible = false;

/** Labels/types for inline add form — order follows UPDATABLE_FIELDS */
const ADD_FORM_FIELD_DEFS = {
    device_id: { label: 'Device ID', required: true, width: '8rem', placeholder: 'ng-xxx' },
    serial_no: { label: 'Serial No', required: false, width: '10rem' },
    model_name: { label: 'Model Name', required: false, width: '12rem', placeholder: 'Mxxxx-**-PoE*' },
    hw_id: { label: 'Hardware ID', required: false, width: '9rem' },
    console_ip: { label: 'Console IP', required: true, width: '10rem', placeholder: '—.—.—.—' },
    port_id: { label: 'Console Port', required: true, width: '7rem', placeholder: 'Port No.', type: 'number', min: 1, max: 64 }
};

function toggleAddForm() {
    const existingForm = document.getElementById('inline-add-form');
    if (existingForm) {
        existingForm.remove();
        addFormVisible = false;
        return;
    }
    
    const tb = tbody();
    if (!tb) return;
    
    const formRow = ce('tr', { id: 'inline-add-form' });
    const formCell = ce('td', { colspan: '10', style: 'padding: 0.8rem; background: var(--panel-bg);' });
    
    const formContainer = ce('div', {
        class: 'add-form-container',
        style: 'display: flex; flex-wrap: wrap; gap: 0.5rem; align-items: flex-end; justify-content: center;'
    });
    
    const fields = UPDATABLE_FIELDS.map(name => ({ name, ...ADD_FORM_FIELD_DEFS[name] }));
    
    const inputs = {};
    
    fields.forEach(field => {
        const fieldGroup = ce('div', { style: 'display: flex; flex-direction: column; gap: 0.2rem;' });
        const label = ce('label', {
            style: 'font-size: 0.75rem; color: var(--muted); font-weight: 600;',
            text: field.label + (field.required ? ' *' : '')
        });
        const input = ce('input', {
            type: field.type || 'text',
            class: 'modal-input',
            style: `width: ${field.width};`,
            placeholder: field.placeholder != null ? field.placeholder : field.label
        });
        if (field.type === 'number') {
            input.min = field.min || 1;
            input.max = field.max || 64;
        }
        if (field.required) input.required = true;
        inputs[field.name] = input;
        fieldGroup.appendChild(label);
        fieldGroup.appendChild(input);
        formContainer.appendChild(fieldGroup);
    });
    
    const btnGroup = ce('div', { style: 'display: flex; gap: 0.4rem; align-items: flex-end; margin-bottom: 0.1rem;' });
    const saveBtn = ce('button', {
        class: 'btn btn-sm btn-success',
        text: '✓ Add',
        type: 'button'
    });
    const cancelBtn = ce('button', {
        class: 'btn btn-sm btn-outline-secondary',
        text: '✕ Cancel',
        type: 'button'
    });
    
    saveBtn.onclick = async () => {
        const device_id = inputs.device_id.value.trim();
        const console_ip = inputs.console_ip.value.trim();
        const port_id = inputs.port_id.value.trim();

        if (!device_id) {
            showToast('Error', 'Device ID is required', { type: 'error' });
            inputs.device_id.focus();
            return;
        }
        if (!console_ip) {
            showToast('Error', 'Console IP is required', { type: 'error' });
            inputs.console_ip.focus();
            return;
        }
        if (!port_id) {
            showToast('Error', 'Console port is required', { type: 'error' });
            inputs.port_id.focus();
            return;
        }

        const portNum = parseInt(port_id, 0);
        if (portNum < 1 || portNum > 64) {
            showToast('Error', 'Port must be between 1-64', { type: 'error' });
            inputs.port_id.focus();
            return;
        }

        const payload = {
            device_id: device_id,
            serial_no: inputs.serial_no.value.trim(),
            model_name: inputs.model_name.value.trim(),
            hw_id: inputs.hw_id.value.trim(),
            console_ip: console_ip,
            port_id: portNum
        };
        
        try {
            saveBtn.disabled = true;
            saveBtn.innerText = 'Adding...';
            const r = await postJSON(API.dbAdd, payload);
            if (r && r.ok) {
                showToast('Success', `Device ${device_id} added`, { type: 'success', autohide: 2000 });
                formRow.remove();
                addFormVisible = false;
                pollAndUpdate(true);
            } else {
                showToast('Error', r.error || 'Failed to add device', { type: 'error', autohide: 3500 });
            }
        } catch (err) {
            showToast('Error', 'Failed to add device', { type: 'error', autohide: 3500 });
        } finally {
            saveBtn.disabled = false;
            saveBtn.innerText = '✓ Add';
        }
    };
    
    cancelBtn.onclick = () => {
        formRow.remove();
        addFormVisible = false;
    };
    
    btnGroup.appendChild(saveBtn);
    btnGroup.appendChild(cancelBtn);
    formContainer.appendChild(btnGroup);
    formCell.appendChild(formContainer);
    formRow.appendChild(formCell);
    tb.appendChild(formRow);
    addFormVisible = true;
    inputs.device_id.focus();
}

/* =========================
 * 13. Export CSV
 * ========================= */
function exportDevicesToCsv(devices) {
    if (!Array.isArray(devices)) return '';
    const header = ['device_id', 'serial_no', 'model_name', 'hw_id', 'mgmt_ip', 'port_id', 'tag', 'current_user', 'duration', 'resv_block', 'health'];
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
 * 14. Wire Top Controls
 * ========================= */
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
    const tBtn = document.getElementById('themeToggle');
    if (tBtn) {
        tBtn.addEventListener('keydown', (e) => {
            if (e.key === 'Enter' || e.key === ' ') {
                e.preventDefault();
                toggleTheme();
            }
        });
    }
})();

/* =========================
 * 15. Boot
 * ========================= */
window.addEventListener('load', () => {
    if (!document.querySelector('#devices_table tbody')) {
        console.warn('bootstrap.js: devices_table tbody not found');
        return;
    }
    
    // Add bottom "Add Device" button if not exists
    const container = document.querySelector('.container-fluid');
    if (container && !document.getElementById('add-device-btn')) {
        const addBtnContainer = ce('div', {
            id: 'add-device-btn',
            style: 'display: flex; justify-content: center; padding: 1rem 0;'
        });
        const addBtn = ce('button', {
            class: 'btn btn-primary btn-sm',
            text: '+ Add Device',
            type: 'button'
        });
        addBtn.onclick = toggleAddForm;
        addBtnContainer.appendChild(addBtn);
        container.appendChild(addBtnContainer);
    }
    
    initFilters();
    startPolling();
    document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'visible') scheduleUpdate(true);
    });
});