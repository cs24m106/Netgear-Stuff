// static/main.js (REPLACE existing file)
// Notes:
// - Two numeric inputs for hours/minutes (only digits allowed).
// - Reserve button is placed in the Actions column (aligned below release where applicable).
// - Health ping: up => next ping after 60s; down => retry every 5s up to 3 times; manual refresh resets retry counter.
// - InputsState persists user typing so live SSE updates don't clear inputs.
// - Minimal, commented for clarity.

const tbody = document.querySelector("#devices-table tbody");
const alertPlaceholder = document.getElementById("alert-placeholder");

// Persist user-typed values keyed by device_name to avoid losing them during re-renders
const inputsState = {}; // { device_name: { owner: "...", hh: "01", mm: "00" } }

// Health state + timers
const healthState = {}; // { device_name: { up: true/false, retries: 0, lastChecked: timestamp } }
const healthTimers = {}; // { device_name: timeoutId }
const MAX_RETRIES = 3;

// Small helpers ------------------------------------------------------------------

// Short alert
function showAlert(msg, type="info", timeout=3000){
  const el = document.createElement("div");
  el.className = `alert alert-${type} mt-2`;
  el.textContent = msg;
  alertPlaceholder.appendChild(el);
  setTimeout(()=> el.remove(), timeout);
}

// format seconds -> HH:MM
function secondsToHHMM(sec){
  if(sec == null) return "";
  const s = Math.max(0, Math.floor(sec));
  const hh = Math.floor(s/3600);
  const mm = Math.floor((s%3600)/60);
  return `${String(hh).padStart(2,'0')}:${String(mm).padStart(2,'0')}`;
}
// format seconds -> HH:MM:SS
function secondsToHHMMSS(sec){
  if(sec == null) return "NA";
  const s = Math.max(0, Math.floor(sec));
  const hh = Math.floor(s / 3600);
  const mm = Math.floor((s % 3600) / 60);
  const ss = s % 60;
  return `${String(hh).padStart(2,'0')}:${String(mm).padStart(2,'0')}:${String(ss).padStart(2,'0')}`;
}

// small copy button (used in mgmt/console cells)
function makeCopyBtn(text, title){
  const btn = document.createElement("button");
  btn.className = "btn btn-sm btn-copy"; // CSS will style smaller and spacing
  btn.type = "button";
  btn.title = title || "Copy";
  btn.innerText = "⧉";
  btn.addEventListener("click", async () => {
    try {
      await navigator.clipboard.writeText(text);
      showAlert("Copied to clipboard", "success", 900);
    } catch (err) {
      showAlert("Copy failed", "danger", 1200);
    }
  });
  return btn;
}

// escape HTML utility for values inserted into attributes/text
function escapeHtml(str){
  if (!str && str !== 0) return "";
  return String(str)
    .replace(/&/g, "&amp;")
    .replace(/"/g, "&quot;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;");
}

// Timestamp Formatter: DD-MM-YY, HH:MM
function formatISO(iso){
  if(!iso || iso === "NA") return "NA";
  try { 
    const d = new Date(iso);
    const day = String(d.getDate()).padStart(2, '0');
    const month = String(d.getMonth() + 1).padStart(2, '0');
    const year = String(d.getFullYear()).slice(-2);
    const hours = String(d.getHours()).padStart(2, '0');
    const mins = String(d.getMinutes()).padStart(2, '0');
    return `${day}-${month}-${year}, ${hours}:${mins}`;
  } catch(e) { return iso; }
}

// MAIN RENDER -------------------------------------------------------------------
function renderDevices(devs){
  const now = Date.now();
  tbody.innerHTML = "";

  devs.forEach(d => {
    const device = d.device_name || "";
    const model = d.model_name || "";
    // show the remote_link as visible hyperlink text (already adjusted earlier)
    const ui = d.remote_link
      ? `<a href="${escapeHtml(d.remote_link)}" target="_blank" rel="noopener noreferrer">${escapeHtml(d.remote_link)}</a>`
      : "";
    const mgmt = d.mgmt_ip || "";
    const consoleVal = `${d.console_ip || ""} ${d.server_port || ""}`.trim();
    const tag = (d.tag || "free").toLowerCase();
    const current_user = d.current_user || "";
    const derived = d._derived || {};
    const reserved = derived.reserved;
    const permanent = derived.permanent;
    const end_iso = derived.end_iso || null;

    // Status badge
    const statusBadge = reserved ? (permanent ? `<span class="badge bg-secondary">STATIC</span>` : `<span class="badge bg-warning text-dark">RESERVED</span>`) : `<span class="badge bg-success">FREE</span>`;

    let reservationHTML = "";
    // Reservation cell: owner + two number boxes (HH / MM) when free; otherwise status info
    if (reserved) {
      // prepare display values; use 'NA' for missing
      const userText = escapeHtml(current_user) || "NA";
      let durText = "NA";
      let leftText = "NA";
      let startText = "NA";
      let endText = "NA";

      // If permanent reservation (resv_end_time == 'NA')
      if (permanent) {
        leftText = "Inf"
      } else if (end_iso) {
        // Format duration and remaining as HH:MM
        durText = escapeHtml(secondsToHHMM(derived.duration_seconds));
        leftText = escapeHtml(secondsToHHMMSS(derived.remaining_seconds));
        startText = escapeHtml(formatISO(derived.start_iso));
        endText = escapeHtml(formatISO(derived.end_iso));
      }

      reservationHTML = `
        <div>
          User: <strong>${userText}</strong>, Duration: ${durText} • ${leftText} left
          <div class="small">Start: ${startText} &nbsp; End: ${endText}</div>
        </div>`;
    } 
    else {
      // restore saved inputs so typing isn't lost on re-renders
      const saved = inputsState[device] || { owner: "", hh: "01", mm: "00" };
      reservationHTML = `
        <div class="d-flex gap-1 align-items-center">
          <input class="form-control form-control-sm w-40 owner-input" name="owner" placeholder="user" value="${escapeHtml(saved.owner)}" />
          <div class="d-flex gap-1 align-items-center duration-group">
            <input class="form-control form-control-sm hh-input" name="hh" inputmode="numeric" pattern="\\d*" maxlength="3" style="width:58px" value="${escapeHtml(saved.hh)}" />
            <span class="small">:</span>
            <input class="form-control form-control-sm mm-input" name="mm" inputmode="numeric" pattern="\\d*" maxlength="2" style="width:46px" value="${escapeHtml(saved.mm)}" />
          </div>
        </div>`;
    }

    // Health cell: cached status or placeholder; include refresh button to manually recheck (resets retry count)
    let healthHTML = "";
    const h = healthState[device];
    if (h && (Date.now() - (h.lastChecked || 0) < 15000)) {
      healthHTML = h.up ? `<span class="text-success">up</span>` : `<span class="text-danger">down</span>`;
    } else {
      healthHTML = `<span class="text-muted">--</span>`;
    }
    // add refresh small button near health
    healthHTML += ` <button class="btn btn-sm btn-health-refresh" data-device="${escapeHtml(device)}" title="Recheck health">⟳</button>`;

    // Actions column: Reserve button for free rows (below) and Release for reserved rows
    let actionsHTML = "";
    if (reserved) {
      actionsHTML = `<button class="btn btn-sm btn-outline-danger release-btn" data-device="${escapeHtml(device)}">Release</button>`;
    } else {
      // Reserve button is placed inside Actions column (aligned under Actions)
      actionsHTML = `<button class="btn btn-sm btn-primary reserve-btn" data-device="${escapeHtml(device)}">Reserve</button>`;
    }

    // Construct table row and cells
    const tr = document.createElement("tr");
    tr.innerHTML = `
      <td>${escapeHtml(device)}</td>
      <td>${escapeHtml(model)}</td>
      <td>${ui}</td>
      <td class="mgmt-cell"></td>
      <td class="console-cell"></td>
      <td class="health-cell">${healthHTML}</td>
      <td>${statusBadge}</td>
      <td class="resv-cell">${reservationHTML}</td>
      <td class="actions-cell">${actionsHTML}</td>
    `;

    // mgmt cell: text + copy button (ssh)
    const mgmtCell = tr.querySelector(".mgmt-cell");
    const mgmtSpan = document.createElement("span");
    mgmtSpan.textContent = mgmt;
    mgmtCell.appendChild(mgmtSpan);
    if (mgmt) {
      mgmtCell.appendChild(makeCopyBtn(`ssh admin@${mgmt}`, `Copy ssh admin@${mgmt}`));
    }

    // console cell: text + copy button (telnet)
    const consoleCell = tr.querySelector(".console-cell");
    const consoleSpan = document.createElement("span");
    consoleSpan.textContent = consoleVal;
    consoleCell.appendChild(consoleSpan);
    if (consoleVal) {
      consoleCell.appendChild(makeCopyBtn(`telnet ${consoleVal}`, `Copy telnet ${consoleVal}`));
    }

    tbody.appendChild(tr);

    // Ensure a health polling schedule exists for this device (creates timers only once)
    ensureHealthPolling(device);
  });

  // After DOM is updated, attach event handlers for inputs and buttons
  bindTableEventHandlers();
}

// EVENT BINDING -----------------------------------------------------------------
function bindTableEventHandlers(){
  // owner / hh / mm inputs - capture changes into inputsState to persist user-typing
  document.querySelectorAll('.resv-cell').forEach(cell => {
    const row = cell.closest('tr');
    if (!row) return;
    const device = row.querySelector('td')?.textContent?.trim();
    if (!device) return;

    const ownerInput = cell.querySelector('.owner-input');
    const hhInput = cell.querySelector('.hh-input');
    const mmInput = cell.querySelector('.mm-input');

    if (ownerInput) {
      ownerInput.oninput = (e) => {
        inputsState[device] = inputsState[device] || { owner: "", hh: "01", mm: "00" };
        inputsState[device].owner = e.target.value;
      };
    }
    if (hhInput) {
      hhInput.oninput = (e) => {
        // allow only digits
        e.target.value = e.target.value.replace(/\D/g, "");
        inputsState[device] = inputsState[device] || { owner: "", hh: "01", mm: "00" };
        inputsState[device].hh = e.target.value || "0";
      };
    }
    if (mmInput) {
      mmInput.oninput = (e) => {
        // allow only digits and limit to 0-59 visually
        e.target.value = e.target.value.replace(/\D/g, "");
        if (e.target.value !== "") {
          let val = parseInt(e.target.value, 10);
          if (isNaN(val)) val = 0;
          if (val > 59) val = 59;
          e.target.value = String(val);
        }
        inputsState[device] = inputsState[device] || { owner: "", hh: "01", mm: "00" };
        inputsState[device].mm = e.target.value || "0";
      };
    }
  });

  // Reserve buttons (in Actions column) - they will read the form inputs from the same row
  document.querySelectorAll('.reserve-btn').forEach(btn => {
    btn.onclick = null;
    btn.onclick = async (ev) => {
      const device = btn.dataset.device;
      const row = btn.closest('tr');
      if (!row) return;
      const ownerInput = row.querySelector('.owner-input');
      const hhInput = row.querySelector('.hh-input');
      const mmInput = row.querySelector('.mm-input');
      const owner = ownerInput ? (ownerInput.value || "").trim() : "";
      const hh = hhInput ? parseInt(hhInput.value || "0", 10) : 0;
      const mm = mmInput ? parseInt(mmInput.value || "0", 10) : 0;
      if (!owner) { showAlert("Enter user name", "warning"); return; }
      const totalMin = (Number.isFinite(hh) ? hh : 0) * 60 + (Number.isFinite(mm) ? mm : 0);
      if (totalMin <= 0) { showAlert("Duration must be > 0", "warning"); return; }

      // Save current inputs (so they persist if reservation fails)
      inputsState[device] = { owner: owner, hh: String(hh).padStart(2,'0'), mm: String(mm).padStart(2,'0') };

      try {
        const resp = await fetch("/api/reserve", {
          method: "POST",
          headers: {"Content-Type":"application/json"},
          body: JSON.stringify({ device_name: device, owner: owner, duration_minutes: totalMin })
        });
        const j = await resp.json();
        if (j.ok) {
          showAlert(`Reserved ${device} for ${owner}`, "success");
          delete inputsState[device]; // clear saved inputs for this device after success
          await loadSnapshot(); // quick sync; SSE will also update soon
        } else {
          showAlert("Reserve failed: " + (j.error || "unknown"), "danger");
        }
      } catch (err) {
        showAlert("Reserve request failed", "danger");
      }
    };
  });

  // Release buttons
  document.querySelectorAll('.release-btn').forEach(btn => {
    btn.onclick = null;
    btn.onclick = async (ev) => {
      const device = btn.dataset.device;
      if (!confirm(`Release ${device}?`)) return;
      try {
        const resp = await fetch("/api/release", {
          method: "POST",
          headers: {"Content-Type":"application/json"},
          body: JSON.stringify({ device_name: device })
        });
        const j = await resp.json();
        if (j.ok) {
          showAlert(`Released ${device}`, "success");
          await loadSnapshot();
        } else {
          showAlert("Release failed: " + (j.error || "unknown"), "danger");
        }
      } catch (err) {
        showAlert("Release request failed", "danger");
      }
    };
  });

  // Health refresh buttons (reset retry counter and re-run ping immediately)
  document.querySelectorAll('.btn-health-refresh').forEach(btn => {
    btn.onclick = null;
    btn.onclick = (ev) => {
      const device = btn.dataset.device;
      // reset retry counter and schedule immediate ping
      healthState[device] = healthState[device] || { up: false, retries: 0, lastChecked: 0 };
      healthState[device].retries = 0;
      // clear any pending timer
      if (healthTimers[device]) {
        clearTimeout(healthTimers[device]);
        delete healthTimers[device];
      }
      // immediately ping
      pingDevice(device);
    };
  });
}

// HEALTH / PING logic -----------------------------------------------------------

// Performs a single ping attempt and schedules next attempt depending on result.
// Respects MAX_RETRIES when down.
async function pingDevice(deviceName){
  // clear any existing timer for this device to avoid duplicates
  if (healthTimers[deviceName]) {
    clearTimeout(healthTimers[deviceName]);
    delete healthTimers[deviceName];
  }

  healthState[deviceName] = healthState[deviceName] || { up: false, retries: 0, lastChecked: 0 };
  try {
    const res = await fetch(`/api/ping?device=${encodeURIComponent(deviceName)}`);
    const j = await res.json();
    if (j.ok) {
      if (j.up) {
        // success: mark up, reset retries
        healthState[deviceName].up = true;
        healthState[deviceName].retries = 0;
        healthState[deviceName].lastChecked = Date.now();
        // schedule next health check after 60s
        healthTimers[deviceName] = setTimeout(() => pingDevice(deviceName), 60000);
      } else {
        // ping returned false (down)
        healthState[deviceName].up = false;
        healthState[deviceName].retries = (healthState[deviceName].retries || 0) + 1;
        healthState[deviceName].lastChecked = Date.now();
        if (healthState[deviceName].retries < MAX_RETRIES) {
          // schedule a quick retry after 5s
          healthTimers[deviceName] = setTimeout(() => pingDevice(deviceName), 5000);
        } else {
          // reached max retries — stop automatic retries until manual refresh
          if (healthTimers[deviceName]) {
            clearTimeout(healthTimers[deviceName]);
            delete healthTimers[deviceName];
          }
        }
      }
    } else {
      // treat as down and retry
      healthState[deviceName].up = false;
      healthState[deviceName].retries = (healthState[deviceName].retries || 0) + 1;
      healthState[deviceName].lastChecked = Date.now();
      if (healthState[deviceName].retries < MAX_RETRIES) {
        healthTimers[deviceName] = setTimeout(() => pingDevice(deviceName), 5000);
      } else {
        if (healthTimers[deviceName]) {
          clearTimeout(healthTimers[deviceName]);
          delete healthTimers[deviceName];
        }
      }
    }
  } catch (err) {
    // network / fetch error -> treat as down and retry
    healthState[deviceName].up = false;
    healthState[deviceName].retries = (healthState[deviceName].retries || 0) + 1;
    healthState[deviceName].lastChecked = Date.now();
    if (healthState[deviceName].retries < MAX_RETRIES) {
      healthTimers[deviceName] = setTimeout(() => pingDevice(deviceName), 5000);
    } else {
      if (healthTimers[deviceName]) {
        clearTimeout(healthTimers[deviceName]);
        delete healthTimers[deviceName];
      }
    }
  } finally {
    // update the visible table row for this device only — easiest is to reload full snapshot quickly
    // (SSE will update soon; fetching snapshot ensures immediate UI feedback)
    // loadSnapshot().catch(()=>{});
  }
}

// Ensure health timer exists for a device (called during render)
function ensureHealthPolling(deviceName){
  if (!healthTimers[deviceName]) {
    // schedule immediate ping (but avoid hammering if recently checked)
    const h = healthState[deviceName];
    const recently = h && ((Date.now() - (h.lastChecked || 0)) < 5000);
    if (!recently) {
      pingDevice(deviceName);
    } else {
      // schedule next based on current state
      const nextInterval = (h && h.up) ? 60000 : (h && h.retries < MAX_RETRIES ? 5000 : null);
      if (nextInterval) {
        healthTimers[deviceName] = setTimeout(() => pingDevice(deviceName), nextInterval);
      }
    }
  }
}

// SNAPSHOT load & SSE ----------------------------------------------------------
async function loadSnapshot(){
  try {
    const res = await fetch("/api/devices");
    const data = await res.json();
    renderDevices(data);
  } catch(err){
    showAlert("Failed to fetch devices", "danger");
  }
}

// SSE live updates for reservation timers (keeps reservation info live)
function connectSSE(){
  const es = new EventSource("/api/events");
  es.onmessage = (ev) => {
    try {
      const devices = JSON.parse(ev.data);
      renderDevices(devices);
    } catch(e){
      console.error("SSE parse", e);
    }
  };
  es.onerror = (e) => {
    console.error("SSE error", e);
  };
}

// bootstrap: initial load, SSE and health polling
loadSnapshot().then(() => {
  connectSSE();
  // Kick off health polling for currently known devices
  fetch("/api/devices").then(r => r.json()).then(list => {
    list.forEach(d => ensureHealthPolling(d.device_name));
  }).catch(()=>{});
});