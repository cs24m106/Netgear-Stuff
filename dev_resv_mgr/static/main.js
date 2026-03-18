// static/main.js
const POLL_INTERVAL = 3000; // ms

async function fetchDevices() {
  const r = await fetch("/api/devices");
  return r.json().then(j => j.devices);
}

async function updateConfigs(mgmt_ip, port_id) {
  if (!mgmt_ip || mgmt_ip === '—.—.—.—') return null;

  try {
    const response = await fetch("/api/config", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ mgmt_ip: mgmt_ip, port_id: port_id})
    });
    
    if (!response.ok) return null;
    
    const data = await response.json();
    return {
      av: `https://localhost:${data.av_port}/`,
      new_main: `https://localhost:${data.new_main_port}/`,
      old_main: `http://localhost:${data.old_main_port}/`,
      console_ip: data.console_ip,
      device_port: data.device_port,
    };
  } catch (e) {
    console.error("Error fetching configs:", e);
    return null;
  }
}

function copyToClipboard(text) {
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(text);
  } else {
    // Fallback
    const ta = document.createElement('textarea');
    ta.value = text;
    document.body.appendChild(ta);
    ta.select();
    document.execCommand('copy');
    ta.remove();
  }
}

async function reserveDevice(device_id, user, hours, minutes) {
  return fetch("/api/reserve", {
    method:"POST",
    headers: {"Content-Type":"application/json"},
    body: JSON.stringify({device_id, user, hours, minutes})
  }).then(r => r.json());
}

async function releaseDevice(device_id) {
  return fetch("/api/release", {
    method:"POST",
    headers: {"Content-Type":"application/json"},
    body: JSON.stringify({device_id})
  }).then(r => r.json());
}

async function removeIP(device_id) {
  return fetch("/api/remove_mgmt_ip", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({device_id})
  }).then(r => r.json());
}

// Create a row (layout structure)
function createRow(d) {
  const tbody = document.querySelector("#devices_table tbody");
  const tr = document.createElement("tr");
  tr.id = `row-${d.device_id}`;

  // 0: Id (Left)
  const tdId = document.createElement('td');
  tdId.className = 'col-left';
  tdId.textContent = d.device_id;
  tr.appendChild(tdId);

  // 1: Model (Left)
  const tdModel = document.createElement('td');
  tdModel.className = 'col-left';
  tdModel.textContent = d.model_name || '';
  tr.appendChild(tdModel);

  // 2: Hardware (Left)
  const tdHw = document.createElement('td');
  tdHw.className = 'col-left';
  tdHw.textContent = d.hw_id || '';
  tr.appendChild(tdHw);

  // 3: UI (Center)
  const tdUi = document.createElement('td');
  tdUi.className = 'col-center';
  tr.appendChild(tdUi);

  // 4: Mgmt-Ip (Left Text, Right Icon)
  const tdMgmt = document.createElement('td');
  tdMgmt.className = 'col-left'; 
  tr.appendChild(tdMgmt);

  // 5: Console-Port (Left Text, Right Icon)
  const tdConsole = document.createElement('td');
  tdConsole.className = 'col-left';
  tr.appendChild(tdConsole);

  // 6: Health (Left Text, Right Icon)
  const tdHealth = document.createElement('td');
  tdHealth.className = 'col-left';
  tr.appendChild(tdHealth);

  // 7: Status (Center)
  const tdStatus = document.createElement('td');
  tdStatus.className = 'col-center';
  tr.appendChild(tdStatus);

  // 8: Reservation (Left - contains Inputs or Info)
  const tdResv = document.createElement('td');
  tdResv.className = 'col-left';
  tr.appendChild(tdResv);

  // 9: Actions (Center - contains Buttons)
  const tdActions = document.createElement('td');
  tdActions.className = 'col-center';
  tr.appendChild(tdActions);

  tbody.appendChild(tr);
  return tr;
}

// Update row cells
function updateRow(d) {
  const tr = document.getElementById(`row-${d.device_id}`) || createRow(d);

  // --- 4: Mgmt-Ip (Flex: Text Left, Icon Right) ---
  const tdMgmt = tr.children[4];
  tdMgmt.innerHTML = '';
  const mgmtContainer = document.createElement('div');
  mgmtContainer.className = 'cell-flex';
  
  const mgmtText = document.createElement('span');
  mgmtText.textContent = d.mgmt_ip || '—.—.—.—';
  mgmtContainer.appendChild(mgmtText);

  if (d.mgmt_ip) {
    // copy btn
    const copyBtn = document.createElement('button');
    copyBtn.className = 'btn icon-only';
    copyBtn.title = `ssh admin@${d.mgmt_ip}`;
    copyBtn.innerText = '⧉';
    copyBtn.onclick = () => copyToClipboard(copyBtn.title);

    // remove btn
    const removeBtn = document.createElement('button');
    removeBtn.className = 'btn icon-only';
    removeBtn.title = 'Remove Mgmt-IP';
    removeBtn.innerText = '✖';
    removeBtn.onclick = async () => {
      const r = await removeIP(d.device_id)
      if (r.ok) {
        alert("Mgmt-IP removed. Use Health-Refresh btn to fetch new mgmt ip via console");
        await fetchAndUpdateSingle(d.device_id);
      } else {
        alert("Failed to remove Mgmt-IP: " + (r.error || "Unknown error"));
      }
    };

    // arrange btns -> order: remove, copy
    mgmtContainer.appendChild(removeBtn);
    mgmtContainer.appendChild(copyBtn);
  }
  tdMgmt.appendChild(mgmtContainer);

  // --- 3: UI (Stacked Links) & 5: Console (Flex: Text Left, Icon Right) ---
  const tdUi = tr.children[3];
  const tdConsole = tr.children[5];
  const portVal = parseInt(d.port_id || '0', 10);
  // We call the async function and handle the result when it arrives
  updateConfigs(d.mgmt_ip, portVal).then(config => {
    tdUi.innerHTML = ''; // Clear previous content
    tdConsole.innerHTML = '';
    if (config) {
      const container = document.createElement('div');
      container.className = 'ui-stack';
      
      container.innerHTML = `
        <a href="${config.av}" target="_blank">AV</a>
        <a href="${config.new_main}" target="_blank">Main(https)</a>
        <a href="${config.old_main}" target="_blank">Main(http)</a>
      `;
      tdUi.appendChild(container);
    } else {
      tdUi.textContent = "——\n——\n——";
    }

    const consContainer = document.createElement('div');
    consContainer.className = 'cell-flex';

    const consText = document.createElement('span');
    consText.textContent = (Number.isNaN(portVal) ? '-' : portVal);
    consContainer.appendChild(consText);

    if (!Number.isNaN(portVal) && config) {
      const copyBtn = document.createElement('button');
      copyBtn.className = 'btn icon-only';
      copyBtn.title = `telnet ${config.console_ip} ${config.device_port}`;
      copyBtn.innerText = '⧉';
      copyBtn.onclick = () => copyToClipboard(copyBtn.title);
      consContainer.appendChild(copyBtn);
    }
    tdConsole.appendChild(consContainer);
      
  });

  // --- 6: Health (Up/Down/Unknown only) ---
  const tdHealth = tr.children[6];
  tdHealth.innerHTML = '';
  const healthContainer = document.createElement('div');
  healthContainer.className = 'cell-flex'; // Use flex to align refresh button right
  
  const healthSpan = document.createElement('span');
  const health = (d.health || 'unk').toLowerCase();
  
  healthSpan.innerText = health; 
  if (health === 'up') healthSpan.className = 'health-up';
  else if (health === 'down') healthSpan.className = 'health-down';
  else if (health === 'busy') healthSpan.className = 'health-busy';
  else healthSpan.className = 'health-unk';
  
  healthContainer.appendChild(healthSpan);

  const refreshBtn = document.createElement('button');
  refreshBtn.className = 'btn icon-only';
  refreshBtn.title = 'Refresh';
  refreshBtn.innerText = '↻';
  refreshBtn.onclick = async () => {
  healthSpan.innerText = '...';
  const r = await fetch("/api/refresh_health", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ device_id: d.device_id })
  });
  const j = await r.json();
  if (j.ok) {
    healthSpan.innerText = j.status;
    if (j.status === 'up') healthSpan.className = 'health-up';
    else if (j.status === 'down') healthSpan.className = 'health-down';
    else if (j.status === 'busy') healthSpan.className = 'health-busy';
    else healthSpan.className = 'health-unk';
  }
};
  healthContainer.appendChild(refreshBtn);
  tdHealth.appendChild(healthContainer);

  // --- 7: Status ---
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
    statusSpan.innerText = tag;
  }
  tdStatus.appendChild(statusSpan);

  // --- 8 & 9: Reservation (Inputs) & Actions (Buttons) ---
  const tdResv = tr.children[8];
  const tdActions = tr.children[9];

  // If user is editing inputs, do NOT overwrite tdResv (check flag on tdResv)
  const isEditing = tdResv.getAttribute('data-editing') === '1';

  if (!isEditing) {
    tdResv.innerHTML = '';
    tdActions.innerHTML = '';

    if (tag === 'free') {
        // --- Reservation Column gets INPUTS ---
        const inputsDiv = document.createElement('div');
        inputsDiv.className = 'resv-inputs';

        const userInp = document.createElement('input');
        userInp.className = 'input-user';
        userInp.placeholder = 'User Name';
        userInp.value = d.current_user || '';
        
        const timeRow = document.createElement('div');
        timeRow.className = 'resv-row';
        
        const hrsInp = document.createElement('input');
        hrsInp.className = 'input-compact';
        hrsInp.type = 'number';
        //hrsInp.min = 0;
        hrsInp.value = 1;
        // Cyclic Logic: 0 <-> 24
        hrsInp.addEventListener('input', function() {
            let val = parseInt(this.value);
            if (val < 0) this.value = 24;
            if (val > 24) this.value = 0;
        });
        
        const minsInp = document.createElement('input');
        minsInp.className = 'input-compact';
        minsInp.type = 'number';
        //minsInp.min = 0;
        //minsInp.max = 59;
        minsInp.value = 0;
        // Cyclic Logic: 0 <-> 59
        minsInp.addEventListener('input', function() {
            let val = parseInt(this.value);
            if (val < 0) this.value = 59;
            if (val > 59) this.value = 0;
        });

        timeRow.appendChild(hrsInp);
        timeRow.appendChild(document.createTextNode('hrs'));
        timeRow.appendChild(minsInp);
        timeRow.appendChild(document.createTextNode('mins'));

        inputsDiv.appendChild(userInp);
        inputsDiv.appendChild(timeRow);
        tdResv.appendChild(inputsDiv);

        // Prevent updates while typing
        [userInp, hrsInp, minsInp].forEach(el => {
            el.addEventListener('focus', () => tdResv.setAttribute('data-editing', '1'));
            el.addEventListener('blur', () => setTimeout(() => tdResv.removeAttribute('data-editing'), 200));
        });

        // --- Actions Column gets RESERVE BUTTON ---
        const reserveBtn = document.createElement('button');
        reserveBtn.className = 'btn action-reserve';
        reserveBtn.innerText = 'RESERVE';
        reserveBtn.onclick = async () => {
            const user = userInp.value.trim();
            const hrs = parseInt(hrsInp.value || '0', 10);
            const mins = parseInt(minsInp.value || '0', 10);
            
            if (!user) { alert("Please enter a user name"); return; }
            
            tdResv.setAttribute('data-editing', '1'); // Lock during request
            const r = await reserveDevice(d.device_id, user, hrs, mins);
            tdResv.removeAttribute('data-editing');
            await fetchAndUpdateSingle(d.device_id);
            if ('ok' in r) {
              alert(`reserve-hostname changed ? ${r.ok}`);
            } else {
              alert(`reserve-hostname change Error : ${r.error}`);
            }
        };
        tdActions.appendChild(reserveBtn);

    } else {
      if (tag === 'resv') {
        // --- Reservation Column gets INFO ---
        const pre = document.createElement('pre');
        pre.style.margin = '0';
        pre.style.fontFamily = 'monospace';
        pre.innerText = d.resv_block || '';
        tdResv.appendChild(pre);

      } else if (tag === 'static') {
        // --- Reservation Column gets Static Info ---
        const pre = document.createElement('pre');
        pre.style.margin = '0';
        pre.style.fontFamily = 'monospace';
        pre.innerText = d.resv_block || 'Static assignment';
        tdResv.appendChild(pre);
      }
      
      // --- Actions Column gets RELEASE BUTTON ---
      tdActions.innerHTML = ''; // Clear the '-'
      const releaseBtn = document.createElement('button');
      releaseBtn.className = 'btn action-release';
      releaseBtn.innerText = 'RELEASE';
      
      releaseBtn.onclick = async () => {
          const r = await releaseDevice(d.device_id);
          await fetchAndUpdateSingle(d.device_id); 
          if ('ok' in r) {
            alert(`release-hostname changed ? ${r.ok}`);
          } else {
            alert(`release-hostname change Error : ${r.error}`);
          }
      };
      tdActions.appendChild(releaseBtn);
    }
  }
}

let devicesCache = {}; 

async function pollAndUpdate() {
  try {
    const devices = await fetchDevices();
    const saw = new Set();
    for (const d of devices) {
      saw.add(d.device_id);
      const prev = devicesCache[d.device_id];
      if (!prev) {
        createRow(d);
        updateRow(d);
      } else {
        // Check for changes
        const fields = ['model_name','hw_id','mgmt_ip','port_id','health','tag','current_user','duration','resv_block'];
        let changed = false;
        for (const f of fields) {
          if ((prev[f]||'') !== (d[f]||'')) { changed = true; break; }
        }
        if (changed) updateRow(d);
      }
      devicesCache[d.device_id] = d;
    }
    // Cleanup deleted
    for (const did in devicesCache) {
      if (!saw.has(did)) {
        const tr = document.getElementById(`row-${did}`);
        if (tr) tr.remove();
        delete devicesCache[did];
      }
    }
  } catch (e) { console.error(e); }
}

async function fetchAndUpdateSingle(device_id) {
    const devices = await fetchDevices();
    const d = devices.find(x => x.device_id === device_id);
    if (d) {
        devicesCache[device_id] = d;
        updateRow(d);
    }
}

// Start
pollAndUpdate();
setInterval(pollAndUpdate, POLL_INTERVAL);