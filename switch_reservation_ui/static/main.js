// static/app.js
const POLL_INTERVAL = 3000; // ms

async function fetchDevices() {
  const r = await fetch("/api/devices");
  const j = await r.json();
  return j.devices;
}

function computeUIUrls(mgmt_ip) {
  if (!mgmt_ip) return null;
  const last = parseInt(mgmt_ip.trim().split('.').slice(-1)[0], 10);
  if (Number.isNaN(last)) return null;
  const av_port = 60000 + last;
  const old_main_port = 50000 + last;
  const new_main_port = 51000 + last;
  return {
    av: `http://localhost:${av_port}/`,
    old_main: `http://localhost:${old_main_port}/`,
    new_main: `http://localhost:${new_main_port}/`
  };
}

function copyToClipboard(text) {
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text);
  } else {
    const ta = document.createElement('textarea');
    ta.value = text;
    document.body.appendChild(ta);
    ta.select();
    document.execCommand('copy');
    ta.remove();
  }
}

async function reserveDevice(device_id, user, hours, minutes) {
  const r = await fetch("/api/reserve", {
    method:"POST",
    headers: {"Content-Type":"application/json"},
    body: JSON.stringify({device_id, user, hours, minutes})
  });
  return r.json();
}
async function releaseDevice(device_id) {
  const r = await fetch("/api/release", {
    method:"POST",
    headers: {"Content-Type":"application/json"},
    body: JSON.stringify({device_id})
  });
  return r.json();
}
async function refreshHealth(device_id) {
  const r = await fetch("/api/refresh_health", {
    method:"POST",
    headers: {"Content-Type":"application/json"},
    body: JSON.stringify({device_id})
  });
  return r.json();
}

// Create a row (first time)
function createRow(d) {
  const tbody = document.querySelector("#devices_table tbody");
  const tr = document.createElement("tr");
  tr.id = `row-${d.device_id}`;

  // Id
  const tdId = document.createElement('td');
  tdId.textContent = d.device_id;
  tr.appendChild(tdId);

  // Model
  const tdModel = document.createElement('td');
  tdModel.textContent = d.model_name || '';
  tr.appendChild(tdModel);

  // Hardware
  const tdHw = document.createElement('td');
  tdHw.textContent = d.hw_id || '';
  tr.appendChild(tdHw);

  // UI (three hyperlinks) - may be null if mgmt_ip invalid/missing
  const tdUi = document.createElement('td');
  tdUi.className = 'left-block';
  tr.appendChild(tdUi);

  // Mgmt-Ip + copy (minimal icon)
  const tdMgmt = document.createElement('td');
  tdMgmt.className = 'left-block';
  tr.appendChild(tdMgmt);

  // Console-Port & telnet copy
  const tdConsole = document.createElement('td');
  tdConsole.className = 'left-block';
  tr.appendChild(tdConsole);

  // Health column (health text + refresh icon)
  const tdHealth = document.createElement('td');
  tdHealth.className = 'left-block';
  tr.appendChild(tdHealth);

  // Status column
  const tdStatus = document.createElement('td');
  tdStatus.className = 'left-block';
  tr.appendChild(tdStatus);

  // Reservation column (preformatted)
  const tdResv = document.createElement('td');
  tdResv.className = 'resv-info left-block';
  tr.appendChild(tdResv);

  // Actions column (inputs + buttons) left aligned
  const tdActions = document.createElement('td');
  tdActions.className = 'left-block';
  tr.appendChild(tdActions);

  tbody.appendChild(tr);

  // for future updates we will keep references
  return tr;
}

// Update row cells (without destroying user input if editing)
function updateRow(d) {
  const tr = document.getElementById(`row-${d.device_id}`) || createRow(d);

  // UI cell
  const tdUi = tr.children[3];
  tdUi.innerHTML = '';
  if (d.mgmt_ip && d.mgmt_ip.trim() !== '') {
    const urls = computeUIUrls(d.mgmt_ip);
    if (urls) {
      const a1 = document.createElement('a'); a1.href = urls.av; a1.innerText = "AV"; a1.target = "_blank";
      const a2 = document.createElement('a'); a2.href = urls.old_main; a2.innerText = "Main(old)"; a2.target = "_blank";
      const a3 = document.createElement('a'); a3.href = urls.new_main; a3.innerText = "Main(new)"; a3.target = "_blank";
      tdUi.appendChild(a1); tdUi.appendChild(document.createTextNode(" | "));
      tdUi.appendChild(a2); tdUi.appendChild(document.createTextNode(" | "));
      tdUi.appendChild(a3);
    } else {
      tdUi.textContent = "—";
    }
  } else {
    tdUi.textContent = "—";
  }

  // Mgmt-Ip cell
  const tdMgmt = tr.children[4];
  tdMgmt.innerHTML = '';
  const mgmtLine = document.createElement('div');
  mgmtLine.textContent = d.mgmt_ip || '-';
  mgmtLine.style.marginBottom = '6px';
  const copySsh = document.createElement('button');
  copySsh.className = 'btn icon-only';
  copySsh.title = `Copy ssh admin@${d.mgmt_ip || ''}`;
  copySsh.innerText = '⧉';
  copySsh.onclick = () => { if (d.mgmt_ip) copyToClipboard(`ssh admin@${d.mgmt_ip}`); };
  mgmtLine.appendChild(copySsh);
  tdMgmt.appendChild(mgmtLine);

  // Console port cell
  const tdConsole = tr.children[5];
  tdConsole.innerHTML = '';
  const portVal = parseInt(d.port_id || '0', 10);
  const consolePort = (Number.isNaN(portVal)) ? '-' : (portVal + 10000);
  const portLine = document.createElement('div');
  portLine.textContent = (Number.isNaN(portVal) ? '-' : portVal);
  portLine.style.marginBottom = '6px';
  const copyTel = document.createElement('button');
  copyTel.className = 'btn icon-only';
  copyTel.title = `Copy telnet ${"192.168.1.102"} ${consolePort}`;
  copyTel.innerText = '⧉';
  copyTel.onclick = () => {
    if (!Number.isNaN(portVal)) copyToClipboard(`telnet ${"192.168.1.102"} ${consolePort}`);
  };
  portLine.appendChild(copyTel);
  tdConsole.appendChild(portLine);

  // Health cell (colorize and include refresh icon, left aligned)
  const tdHealth = tr.children[6];
  tdHealth.innerHTML = '';
  const healthDiv = document.createElement('div');
  const healthSpan = document.createElement('span');
  const health = (d.health || 'unknown').toLowerCase();
  if (health === 'up') {
    healthSpan.className = 'health-up';
    healthSpan.innerText = 'up';
  } else if (health === 'down') {
    healthSpan.className = 'health-down';
    healthSpan.innerText = 'down' + (d.retry_count ? ` (${d.retry_count})` : '');
  } else {
    healthSpan.className = 'health-unknown';
    healthSpan.innerText = 'unknown';
  }
  healthDiv.appendChild(healthSpan);

  const refreshBtn = document.createElement('button');
  refreshBtn.className = 'btn icon-only';
  refreshBtn.title = 'Refresh health';
  refreshBtn.innerText = '↻';
  refreshBtn.style.marginLeft = '8px';
  refreshBtn.onclick = async () => {
    await fetch("/api/refresh_health", {method:"POST", headers: {"Content-Type":"application/json"}, body: JSON.stringify({device_id:d.device_id})});
    // small optimistic UI change
    healthSpan.innerText = 'checking...';
    setTimeout(()=> { /* next poll will update */ }, 400);
  };
  healthDiv.appendChild(refreshBtn);
  tdHealth.appendChild(healthDiv);

  // Status cell (colorize)
  const tdStatus = tr.children[7];
  tdStatus.innerHTML = '';
  const tag = (d.tag || 'free').toLowerCase();
  const statusSpan = document.createElement('span');
  if (tag === 'free') {
    statusSpan.className = 'status-free';
    statusSpan.innerText = 'Free';
  } else if (tag === 'resv') {
    statusSpan.className = 'status-resv';
    statusSpan.innerText = 'Reserved';
  } else if (tag === 'static') {
    statusSpan.className = 'status-static';
    statusSpan.innerText = 'Static';
  } else {
    statusSpan.className = 'status-static';
    statusSpan.innerText = tag;
  }
  tdStatus.appendChild(statusSpan);

  // Reservation cell — use server-provided resv_block string (keep newlines)
  const tdResv = tr.children[8];
  tdResv.innerHTML = '';
  if (d.resv_block && d.resv_block.trim() !== '') {
    const pre = document.createElement('pre');
    pre.style.margin = '0';
    pre.style.fontFamily = 'monospace';
    pre.style.whiteSpace = 'pre-line';
    pre.textContent = d.resv_block;
    tdResv.appendChild(pre);
  } else {
    // empty (free) — will show input fields in actions column instead
    tdResv.textContent = '';
  }

  // Actions column: show inputs / buttons depending on tag
  const tdActions = tr.children[9];
  // If user is actively editing this row, do NOT replace inputs (so we preserve focus/typing)
  const isEditing = tdActions.getAttribute('data-editing') === '1';
  if (isEditing) {
    // skip overwriting while editing
    return;
  }
  tdActions.innerHTML = '';

  const leftBlock = document.createElement('div');
  leftBlock.style.display = 'flex';
  leftBlock.style.flexDirection = 'row';
  leftBlock.style.alignItems = 'center';
  leftBlock.style.gap = '6px';

  if (tag === 'free') {
    // show inputs: user, hours, minutes, RESERVE button
    const userInput = document.createElement('input');
    userInput.className = 'input-user';
    userInput.placeholder = 'user';
    userInput.value = d.current_user || '';

    const hoursInput = document.createElement('input');
    hoursInput.className = 'input-compact';
    hoursInput.type = 'number';
    hoursInput.min = 0;
    hoursInput.value = '1'; // default 1 hr
    const hoursLabel = document.createElement('span'); hoursLabel.innerText = 'hrs';

    const minsInput = document.createElement('input');
    minsInput.className = 'input-compact';
    minsInput.type = 'number';
    minsInput.min = 0;
    minsInput.max = 59;
    minsInput.value = '0';
    const minsLabel = document.createElement('span'); minsLabel.innerText = 'mins';

    const reserveBtn = document.createElement('button');
    reserveBtn.className = 'btn small action-reserve';
    reserveBtn.innerText = 'RESERVE';
    reserveBtn.onclick = async () => {
      const user = userInput.value.trim();
      const hours = parseInt(hoursInput.value || '0', 10) || 0;
      const minutes = parseInt(minsInput.value || '0', 10) || 0;
      if (!user) { alert("Enter user"); return; }
      // set editing flag to prevent overwriting inputs during request
      tdActions.setAttribute('data-editing', '1');
      await reserveDevice(d.device_id, user, hours, minutes);
      tdActions.removeAttribute('data-editing');
      await fetchAndUpdateSingle(d.device_id);
    };

    // set editing flag when focused so poll doesn't overwrite them
    [userInput, hoursInput, minsInput].forEach(inp => {
      inp.addEventListener('focus', () => tdActions.setAttribute('data-editing', '1'));
      inp.addEventListener('blur', () => setTimeout(()=>tdActions.removeAttribute('data-editing'), 200));
    });

    leftBlock.appendChild(userInput);
    leftBlock.appendChild(hoursInput); leftBlock.appendChild(hoursLabel);
    leftBlock.appendChild(minsInput); leftBlock.appendChild(minsLabel);
    leftBlock.appendChild(reserveBtn);

  } else if (tag === 'resv') {
    // show RELEASE button only
    const releaseBtn = document.createElement('button');
    releaseBtn.className = 'btn small action-release';
    releaseBtn.innerText = 'RELEASE';
    releaseBtn.onclick = async () => {
      tdActions.setAttribute('data-editing', '1');
      await releaseDevice(d.device_id);
      tdActions.removeAttribute('data-editing');
      await fetchAndUpdateSingle(d.device_id);
    };
    leftBlock.appendChild(releaseBtn);
  } else if (tag === 'static') {
    // no action buttons for static; show owner
    const owner = document.createElement('div');
    owner.textContent = d.current_user || '-';
    leftBlock.appendChild(owner);
  } else {
    leftBlock.appendChild(document.createTextNode('-'));
  }

  tdActions.appendChild(leftBlock);
}

// fetch devices and update rows selectively
let devicesCache = {}; // device_id -> device object

async function pollAndUpdate() {
  try {
    const devices = await fetchDevices();
    const saw = new Set();
    for (const d of devices) {
      saw.add(d.device_id);
      // compare with cache, if changed update row
      const prev = devicesCache[d.device_id];
      if (!prev) {
        // new row
        createRow(d);
        updateRow(d);
      } else {
        // shallow compare relevant fields to decide update
        const fieldsToCheck = ['model_name','hw_id','mgmt_ip','port_id','health','retry_count','tag','current_user','duration','resv_block'];
        let changed = false;
        for (const f of fieldsToCheck) {
          if ((prev[f] || '') !== (d[f] || '')) { changed = true; break; }
        }
        if (changed) updateRow(d);
      }
      devicesCache[d.device_id] = d;
    }
    // remove deleted rows
    for (const did in devicesCache) {
      if (!saw.has(did)) {
        const tr = document.getElementById(`row-${did}`);
        if (tr) tr.remove();
        delete devicesCache[did];
      }
    }
  } catch (e) {
    console.error("poll/update error", e);
  }
}

async function fetchAndUpdateSingle(device_id) {
  try {
    const resp = await fetch("/api/devices");
    const json = await resp.json();
    const devices = json.devices || [];
    for (const d of devices) {
      if (d.device_id === device_id) {
        devicesCache[device_id] = d;
        updateRow(d);
        break;
      }
    }
  } catch (e) { console.error(e); }
}

// initial load
pollAndUpdate();
setInterval(pollAndUpdate, POLL_INTERVAL);