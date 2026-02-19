// static/main.js
// Minimal, Bootstrap-friendly frontend for the Switch Management UI.
// - Renders /api/devices into #devices_table
// - Small reserve/release/refresh actions wired to endpoints
// - Theme toggle + bootstrapify helper
// - Polling loop to keep the UI fresh

(() => {
  "use strict";

  // -------------------------
  // Configurable constants
  // -------------------------
  const API_DEVICES = "/api/devices";
  const API_RESERVE = "/api/reserve";
  const API_RELEASE = "/api/release";
  const API_REFRESH_HEALTH = "/api/refresh_health";
  const POLL_INTERVAL_MS = 5000; // keep moderate to avoid excessive load

  // -------------------------
  // Theme toggle (Bootstrap 5.3 data-bs-theme)
  // -------------------------
  /* CHANGE: theme helper + persisted preference */
  const THEME_KEY = "switch_ui_theme";
  const root = document.documentElement;
  function setTheme(name) {
    if (name === "dark") root.setAttribute("data-bs-theme", "dark");
    else root.removeAttribute("data-bs-theme");
  }
  function getSavedTheme() {
    return localStorage.getItem(THEME_KEY);
  }
  function saveTheme(name) {
    localStorage.setItem(THEME_KEY, name);
  }

  // initialize theme on load
  (function initTheme() {
    const saved = getSavedTheme();
    if (saved) {
      setTheme(saved);
    } else if (window.matchMedia && window.matchMedia("(prefers-color-scheme: dark)").matches) {
      setTheme("dark");
    } else {
      setTheme("light");
    }
  })();

  // -------------------------
  // Bootstrap helper (apply bootstrap classes to action buttons)
  // -------------------------
  /* CHANGE: bootstrapifyButtons helper */
  window.bootstrapifyButtons = function bootstrapifyButtons(scope = document) {
    try {
      scope.querySelectorAll(".action-reserve:not(.btn)").forEach((b) => {
        b.classList.add("btn", "btn-sm", "btn-success", "action-btn");
      });
      scope.querySelectorAll(".action-release:not(.btn)").forEach((b) => {
        b.classList.add("btn", "btn-sm", "btn-outline-warning", "action-btn");
      });
      scope.querySelectorAll(".action-refresh:not(.btn)").forEach((b) => {
        b.classList.add("btn", "btn-sm", "btn-outline-secondary", "action-btn");
      });
      scope.querySelectorAll(".action-config:not(.btn)").forEach((b) => {
        b.classList.add("btn", "btn-sm", "btn-outline-primary", "action-btn");
      });
    } catch (e) {
      // non-fatal
      console.error("bootstrapifyButtons error:", e);
    }
  };

  // -------------------------
  // UI helpers
  // -------------------------
  function qs(sel, ctx = document) { return ctx.querySelector(sel); }
  function ce(tag, props = {}, ...children) {
    const el = document.createElement(tag);
    for (const k in props) {
      if (k === "class") el.className = props[k];
      else if (k === "html") el.innerHTML = props[k];
      else el.setAttribute(k, props[k]);
    }
    for (const ch of children) {
      if (ch == null) continue;
      if (typeof ch === "string") el.appendChild(document.createTextNode(ch));
      else el.appendChild(ch);
    }
    return el;
  }

  // -------------------------
  // HTTP helpers
  // -------------------------
  async function postJSON(url, body) {
    const resp = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    if (!resp.ok) {
      const text = await resp.text().catch(() => "");
      throw new Error(`HTTP ${resp.status}: ${text}`);
    }
    return resp.json().catch(() => ({}));
  }

  async function fetchDevices() {
    const r = await fetch(API_DEVICES, { cache: "no-store" });
    if (!r.ok) throw new Error("Failed to fetch devices: " + r.status);
    return r.json();
  }

  // -------------------------
  // Row creation + Actions
  // -------------------------
  function createActionButton(label, cls, title) {
    const b = ce("button", { class: cls, type: "button", title });
    b.textContent = label;
    return b;
  }

  // Keep row rendering stable so we don't break existing markup the server expects
  function createRowForDevice(d) {
    // Expected fields (based on server): device_id, model_name, mgmt_ip, port_id, tag, current_user, duration, resv_block, health, retry_count
    const tr = ce("tr", { id: `dev-${d.device_id}` });

    // device id
    tr.appendChild(ce("td", { class: "word-wrap col-left" }, d.device_id || ""));

    // model
    tr.appendChild(ce("td", { class: "word-wrap col-left" }, d.model_name || ""));

    // mgmt ip
    tr.appendChild(ce("td", { class: "col-center" }, d.mgmt_ip || ""));

    // port id
    tr.appendChild(ce("td", { class: "col-center" }, d.port_id || ""));

    // tag (free/resv/static)
    tr.appendChild(ce("td", { class: "col-center" }, d.tag || ""));

    // current_user
    tr.appendChild(ce("td", { class: "col-left" }, d.current_user || ""));

    // duration
    tr.appendChild(ce("td", { class: "col-center" }, d.duration || ""));

    // reservation block (long text)
    tr.appendChild(ce("td", { class: "word-wrap col-left" }, d.resv_block || ""));

    // health
    const healthCell = ce("td", { class: "col-center" }, d.health || "unk");
    tr.appendChild(healthCell);

    // retry_count
    tr.appendChild(ce("td", { class: "col-center" }, String(d.retry_count || 0)));

    // Actions cell
    const actions = ce("td", { class: "col-center" });

    // Reserve button
    const reserveBtn = createActionButton("Reserve", "action-reserve", "Reserve this device");
    reserveBtn.addEventListener("click", () => onReserveClick(d));
    actions.appendChild(reserveBtn);

    // Release button
    const releaseBtn = createActionButton("Release", "action-release", "Release reservation");
    releaseBtn.addEventListener("click", () => onReleaseClick(d));
    actions.appendChild(releaseBtn);

    // Refresh health
    const refreshBtn = createActionButton("Refresh Health", "action-refresh", "Refresh health for this device");
    refreshBtn.addEventListener("click", () => onRefreshHealthClick(d));
    actions.appendChild(refreshBtn);

    // Config (optional): get computed ports etc (light action)
    const confBtn = createActionButton("Config", "action-config", "Show computed ports (console)");
    confBtn.addEventListener("click", () => onConfigClick(d));
    actions.appendChild(confBtn);

    tr.appendChild(actions);

    // Apply bootstrap classes to the action buttons in this row (CHANGE)
    bootstrapifyButtons(tr); // CHANGE: ensure bootstrap styling for row-level buttons

    return tr;
  }

  // -------------------------
  // Action handlers (minimal prompts)
  // -------------------------
  async function onReserveClick(d) {
    // Minimal prompt-based reservation UI — replace with modal if you prefer.
    const username = prompt("Reserve - enter username (exact):", (d.current_user || "").trim());
    if (!username) return;
    let minutes = prompt("Duration minutes (total):", "60");
    if (minutes === null) return;
    minutes = parseInt(minutes, 10) || 0;
    const hours = Math.floor(minutes / 60);
    const mins = minutes % 60;

    // Prepare payload
    try {
      await postJSON(API_RESERVE, { device_id: d.device_id, user: username, hours, minutes: mins });
      // feedback: optimistic UI refresh
      await pollAndUpdate(true);
    } catch (err) {
      alert("Reserve failed: " + (err.message || err));
      console.error(err);
    }
  }

  async function onReleaseClick(d) {
    if (!confirm(`Release ${d.device_id} (currently: ${d.current_user || "none"})?`)) return;
    try {
      await postJSON(API_RELEASE, { device_id: d.device_id });
      await pollAndUpdate(true);
    } catch (err) {
      alert("Release failed: " + (err.message || err));
      console.error(err);
    }
  }

  async function onRefreshHealthClick(d) {
    try {
      await postJSON(API_REFRESH_HEALTH, { device_id: d.device_id });
      // immediate local poll to reflect possible state change
      await pollAndUpdate(true);
    } catch (err) {
      console.error("refresh health failed", err);
      alert("Refresh failed: " + (err.message || err));
    }
  }

  async function onConfigClick(d) {
    // Light informational action — ask backend for details via /api/config (POST)
    if (!d.mgmt_ip || !d.port_id) {
      alert("Missing mgmt_ip or port_id");
      return;
    }
    try {
      const resp = await postJSON("/api/config", { mgmt_ip: d.mgmt_ip, port_id: d.port_id });
      const msg = [
        `Switch ID: ${resp.switch_id || "-"}`,
        `AV Port: ${resp.av_port || "-"}`,
        `New Main Port: ${resp.new_main_port || "-"}`,
        `Old Main Port: ${resp.old_main_port || "-"}`,
        `Console IP: ${resp.console_ip || "-"}`,
        `Device Port: ${resp.device_port || "-"}`
      ].join("\n");
      alert(msg);
    } catch (err) {
      console.error(err);
      alert("Config fetch failed: " + (err.message || err));
    }
  }

  // -------------------------
  // Rendering / diff update
  // -------------------------
  function clearTableBody(tbl) {
    const tbody = tbl.tBodies[0] || tbl.createTBody();
    tbody.innerHTML = "";
    return tbody;
  }

  function renderDevicesTable(devices) {
    const tbl = qs("#devices_table");
    if (!tbl) {
      console.warn("#devices_table not found in DOM");
      return;
    }
    const tbody = clearTableBody(tbl);

    // Keep the original column order and structure minimalistic
    devices.forEach((d) => {
      const tr = createRowForDevice(d);
      tbody.appendChild(tr);
    });

    // Ensure all action buttons (globally) are styled (safety)
    bootstrapifyButtons(document); // CHANGE: ensure global conversion after full render
  }

  // -------------------------
  // Polling
  // -------------------------
  let pollTimer = null;
  let isPolling = false;

  async function pollAndUpdate(force = false) {
    if (isPolling && !force) return;
    isPolling = true;
    try {
      const data = await fetchDevices();
      if (!data || !Array.isArray(data.devices)) {
        console.warn("Unexpected devices structure", data);
        return;
      }
      renderDevicesTable(data.devices);
    } catch (err) {
      console.error("pollAndUpdate error:", err);
    } finally {
      isPolling = false;
    }
  }

  function startPolling() {
    if (pollTimer) clearInterval(pollTimer);
    pollAndUpdate(true);
    pollTimer = setInterval(() => pollAndUpdate(), POLL_INTERVAL_MS);
  }

  // -------------------------
  // Wire up UI controls on DOM ready
  // -------------------------
  document.addEventListener("DOMContentLoaded", () => {
    // Theme toggle button (CHANGE: binds theme toggle added in index.html)
    const themeBtn = qs("#themeToggle");
    if (themeBtn) {
      // set button pressed state
      const cur = root.getAttribute("data-bs-theme") === "dark" ? "dark" : "light";
      themeBtn.setAttribute("aria-pressed", cur === "dark" ? "true" : "false");
      themeBtn.addEventListener("click", () => {
        const nowTheme = root.getAttribute("data-bs-theme") === "dark" ? "dark" : "light";
        const next = nowTheme === "dark" ? "light" : "dark";
        setTheme(next);
        saveTheme(next);
        themeBtn.setAttribute("aria-pressed", next === "dark" ? "true" : "false");
      });
    }

    // Global refresh all (button added in index.html). Calls pollAndUpdate.
    const refreshAllBtn = qs("#refreshAllBtn");
    if (refreshAllBtn) {
      refreshAllBtn.addEventListener("click", () => {
        pollAndUpdate(true);
        // brief button feedback
        refreshAllBtn.disabled = true;
        setTimeout(() => (refreshAllBtn.disabled = false), 900);
      });
    }

    // Bootstrapify any buttons that were part of static HTML
    bootstrapifyButtons(document); // CHANGE: initial pass

    // Start polling
    startPolling();
  });

  // expose some functions for debugging from console (optional)
  window.pollAndUpdate = pollAndUpdate;
  window.startPolling = startPolling;
})();