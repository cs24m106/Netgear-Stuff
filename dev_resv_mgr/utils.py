"""Utilities: audit logging, nested-JSON DB store, health manager, SSH/console helpers, reservation monitor.

In-memory devices are kept FLAT (one dict per device with all columns) so existing
callers and the frontend keep working unchanged. Persistence to ``database.json``
splits each device into the ``static`` / ``dynm`` / ``resv`` groups defined in
``config``.
"""

import datetime
import json
import re
import threading
import time
import traceback
import paramiko
from filelock import FileLock

import config
from config import *

# -------------------------------------------------------------------------------------------------
# Audit logging (operations.log — used by api, app; config shims call these via deferred import)
# -------------------------------------------------------------------------------------------------
def log_operation(operation, device_id, changes, user="system"):
    """
    Log all database operations for audit/revert capability.
    operation: ADD, EDIT, DELETE, or LOG (structured app/audit messages).
    Format: JSON lines with timestamp, operation, device_id, changes, user.
    """
    lock = FileLock(LOG_LOCK, timeout=10)
    with lock:
        timestamp = datetime.datetime.utcnow().isoformat()
        log_entry = {
            "timestamp": timestamp,
            "operation": operation,
            "device_id": device_id,
            "changes": changes,
            "user": user,
        }
        with open(LOG_PATH, "a", newline="") as f:
            f.write(json.dumps(log_entry) + "\n")


LOG_LEVELS = frozenset({"INFO", "WARNING", "ERROR"})


def log_application(level, message, device_id="-", user="system", **extra):
    """
    Operation type LOG with level in changes: INFO, WARNING, or ERROR.
    message is human-readable; extra keys are merged into changes for querying.
    """
    lvl = (level or "INFO").upper()
    if lvl not in LOG_LEVELS:
        lvl = "INFO"
    changes = {"level": lvl, "message": message}
    if extra:
        changes.update(extra)
    log_operation("LOG", device_id, changes, user=user)


def reservation_actor_user(body=None, device_row=None):
    """
    Username for reservation-related audit lines: POST 'user' if present and non-empty,
    else the device's current_user. Used as operations.log top-level 'user' for filtering.
    """
    if body:
        u = body.get("user")
        if u is not None and str(u).strip():
            return str(u).strip()
    if device_row:
        u = (device_row.get("current_user") or "").strip()
        if u:
            return u
    return "system"


# -------------------------------------------------------------------------------------------------
# Nested-JSON DB store
# -------------------------------------------------------------------------------------------------
def _flatten(entry):
    """Nested {static, dynm, resv} -> single flat dict with all known fields filled in."""
    out = {}
    for grp in ("static", "dynm", "resv"):
        out.update(entry.get(grp) or {})
    for f in ALL_FIELDS:
        out.setdefault(f, "")
    if not out.get("tag"):
        out["tag"] = "free"
    return out


def _split_groups(flat):
    """Flat dict -> nested {static, dynm, resv} using the field maps in config."""
    return {
        "static": {f: flat.get(f, "") for f in STATIC_FIELDS},
        "dynm": {f: flat.get(f, "") for f in DYNM_FIELDS},
        "resv": {f: flat.get(f, "") for f in RESV_FIELDS},
    }


def _read_db_raw():
    """Return the full parsed JSON (with ``lab`` and ``devices`` keys preserved)."""
    with open(DB_PATH, "r", encoding="utf-8") as f:
        return json.load(f) or {}


def _write_db_raw(data):
    """Write the whole DB doc back as pretty + one-line groups (matches current style)."""
    devices = data.get("devices") or []
    lines = ["{"]
    if "lab" in data:
        lines.append(f'  "lab": {json.dumps(data["lab"], ensure_ascii=False)},')
    lines.append('  "devices": [')
    for i, entry in enumerate(devices):
        comma = "," if i < len(devices) - 1 else ""
        lines.append("    {")
        lines.append(f'      "static": {json.dumps(entry.get("static", {}), ensure_ascii=False)},')
        lines.append(f'      "dynm": {json.dumps(entry.get("dynm", {}), ensure_ascii=False)},')
        lines.append(f'      "resv": {json.dumps(entry.get("resv", {}), ensure_ascii=False)}')
        lines.append(f"    }}{comma}")
    lines.append("  ]")
    lines.append("}")
    with open(DB_PATH, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def read_devices():
    """Return a list of FLAT device dicts (one per row)."""
    lock = FileLock(DB_LOCK, timeout=10)
    with lock:
        data = _read_db_raw()
    return [_flatten(entry) for entry in (data.get("devices") or [])]


def write_devices(devices):
    """Persist the full devices list, re-grouping each flat row into static/dynm/resv."""
    lock = FileLock(DB_LOCK, timeout=10)
    with lock:
        data = _read_db_raw()
        data["devices"] = [_split_groups(d) for d in devices]
        _write_db_raw(data)


def _update_group(_device_id, _group_name, _allowed_fields, **fields):
    """
    Rewrite ONLY the named group for one device on disk. Static is intentionally
    not exposed here -- the static block is set at add-time and never edited via
    these helpers.

    Returns the resulting flat row, or None if the device doesn't exist.
    """
    bad = [k for k in fields if k not in _allowed_fields]
    if bad:
        raise ValueError(f"fields {bad!r} are not in '{_group_name}' group {_allowed_fields!r}")

    lock = FileLock(DB_LOCK, timeout=10)
    with lock:
        data = _read_db_raw()
        for entry in (data.get("devices") or []):
            if (entry.get("static") or {}).get("device_id") == _device_id:
                grp = entry.setdefault(_group_name, {})
                for k, v in fields.items():
                    grp[k] = v
                _write_db_raw(data)
                return _flatten(entry)
    return None


def update_dynm(_device_id, **fields):
    """Update one or more 'dynm' fields (mgmt_ip / console_ip / port_id) atomically."""
    return _update_group(_device_id, "dynm", DYNM_FIELDS, **fields)


def update_resv(_device_id, **fields):
    """Update one or more 'resv' fields (tag / current_user / duration / resv_end_time) atomically."""
    return _update_group(_device_id, "resv", RESV_FIELDS, **fields)


def add_device(flat):
    """Append a device. The 'static' group is taken as-is from ``flat`` and never
    edited again afterward. dynm/resv default sensibly."""
    lock = FileLock(DB_LOCK, timeout=10)
    with lock:
        data = _read_db_raw()
        devices = data.setdefault("devices", [])
        if any((e.get("static") or {}).get("device_id") == flat.get("device_id") for e in devices):
            raise ValueError(f"device_id {flat.get('device_id')!r} already exists")
        new_entry = _split_groups({**{f: "" for f in ALL_FIELDS}, **flat, "tag": flat.get("tag") or "free"})
        devices.append(new_entry)
        _write_db_raw(data)
        return _flatten(new_entry)


def delete_device(device_id):
    """Remove a device by id. Returns the deleted flat row, or None if not found."""
    lock = FileLock(DB_LOCK, timeout=10)
    with lock:
        data = _read_db_raw()
        devices = data.get("devices") or []
        kept, removed = [], None
        for entry in devices:
            if (entry.get("static") or {}).get("device_id") == device_id and removed is None:
                removed = _flatten(entry)
            else:
                kept.append(entry)
        if removed is None:
            return None
        data["devices"] = kept
        _write_db_raw(data)
        return removed


# -------------------------------------------------------------------------------------------------
# Health manager (single SSH session -> sequential pings)
# -------------------------------------------------------------------------------------------------

def init_device_state_from_db():
    devices = read_devices()
    with device_state_lock:
        for d in devices:
            did = d["device_id"]
            if did not in device_state:
                device_state[did] = {
                    "health": "unk", "retry_count": 0,
                    "next_check_ts": time.time() + 1
                }
    log_app("INFO", "[INFO] device_state initialized from DB", extra={"device_count": len(devices)})


class HealthManager(threading.Thread):
    def __init__(self, officeIP, ssh_user, ssh_password):
        super().__init__(daemon=True)
        self.officeIP = officeIP
        self.ssh_user = ssh_user
        self.ssh_password = ssh_password
        self.keep_running = True
        self.ssh_client = None
        self.ssh_lock = threading.Lock()
        self.reconnect_backoff = 5

    def ensure_ssh(self):
        with self.ssh_lock:
            if self.ssh_client is not None:
                return True
            try:
                client = paramiko.SSHClient()
                client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
                client.connect(self.officeIP, username=self.ssh_user, password=self.ssh_password, timeout=10)
                self.ssh_client = client
                log_app(
                    "INFO", f"[INFO] HealthManager SSH connected to {self.officeIP}",
                    extra={"component": "health", "host": self.officeIP},
                )
                return True
            except Exception as e:
                self.ssh_client = None
                log_app(
                    "WARNING", f"[WARNING] HealthManager SSH connect failed: {self.officeIP}",
                    extra={"component": "health", "host": self.officeIP, "error": str(e)},
                )
                return False

    def close_ssh(self):
        with self.ssh_lock:
            try:
                if self.ssh_client:
                    self.ssh_client.close()
            finally:
                self.ssh_client = None

    def run_one_cycle(self, check_list):
        if not check_list:
            return

        if not self.ensure_ssh():
            log_app(
                "WARNING", "[WARNING] health check cycle: SSH unavailable; marking candidates down",
                extra={"component": "health", "device_ids": [d[0] for d in check_list]},
            )
            for device_id, mgmt_ip in check_list:
                with device_state_lock:
                    st = device_state.get(device_id)
                    if not st:
                        continue
                    st["retry_count"] += 1
                    st["health"] = "down"
                    st["next_check_ts"] = time.time() + DOWN_HEALTH_TIMER
            return

        log_app(
            "INFO", f"[INFO] health check cycle: pinging {len(check_list)} device(s)",
            extra={"component": "health", "checks": [{"device_id": did, "mgmt_ip": mip} for did, mip in check_list]},
        )
        with self.ssh_lock:
            client = self.ssh_client
            for device_id, mgmt_ip in check_list:
                try:
                    ok = ssh_ping_once(client, mgmt_ip)
                except Exception as e:
                    ok = False
                    log_app(
                        "ERROR", f"[ERROR] health ping exception device_id={device_id} mgmt={mgmt_ip}",
                        device_id=device_id, extra={"component": "health", "mgmt_ip": mgmt_ip, "error": str(e)},
                    )
                with device_state_lock:
                    st = device_state.get(device_id)
                    if not st:
                        continue
                    if ok:
                        st["health"] = "up"
                        st["retry_count"] = 0
                        st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
                        log_app(
                            "INFO", f"[INFO] health result UP (ping ok) mgmt={mgmt_ip}",
                            device_id=device_id, extra={"component": "health", "mgmt_ip": mgmt_ip},
                        )
                    else:
                        st["retry_count"] += 1
                        st["health"] = "down"
                        if st["retry_count"] < MAX_HEALTH_CHECK_RETRIES:
                            st["next_check_ts"] = time.time() + DOWN_HEALTH_TIMER
                        else:
                            st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
                        lvl = "WARNING" if st["retry_count"] >= MAX_HEALTH_CHECK_RETRIES else "INFO"
                        log_app(
                            lvl, f"[{lvl}] health result DOWN mgmt={mgmt_ip} retry_count={st['retry_count']}",
                            device_id=device_id, extra={"component": "health", "mgmt_ip": mgmt_ip, "retry_count": st["retry_count"]},
                        )

    def run(self):
        log_app("INFO", "[INFO] HealthManager thread started", extra={"component": "health"})
        while self.keep_running:
            try:
                now = time.time()
                check_list_ids = []
                with device_state_lock:
                    for did, st in device_state.items():
                        if st["next_check_ts"] <= now:
                            check_list_ids.append(did)
                if check_list_ids:
                    db_devices = read_devices()
                    mgmt_map = {d["device_id"]: d["mgmt_ip"] for d in db_devices}
                    to_ping = []
                    for did in check_list_ids:
                        mgmt = mgmt_map.get(did)
                        if mgmt and is_valid_ipv4(mgmt):
                            to_ping.append((did, mgmt))
                        else:
                            with device_state_lock:
                                st = device_state.get(did)
                                if st:
                                    st["health"] = "unk"
                                    st["retry_count"] = 0
                                    st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
                            log_app(
                                "INFO", "[INFO] health skip (no valid mgmt_ip); state set unk",
                                device_id=did, extra={"component": "health", "mgmt_ip": mgmt},
                            )
                    self.run_one_cycle(to_ping)
                else:
                    time.sleep(0.5)
            except Exception as e:
                traceback.print_exc()
                log_app(
                    "ERROR", f"[ERROR] HealthManager.run loop exception: {e}",
                    extra={"component": "health"},
                )
                self.close_ssh()
                time.sleep(self.reconnect_backoff)

    def stop(self):
        self.keep_running = False
        self.close_ssh()


health_manager = HealthManager(SSH_IP, SSH_USER, SSH_PSWD)


# -------------------------------------------------------------------------------------------------
# Telnet & SSH helpers (interact with console via singleton SSH client shell)
# -------------------------------------------------------------------------------------------------

def ssh_ping_once(ssh_client, mgmt_ip, count=2, timeout=6):
    try:
        cmd = f"ping -c {count} -w {timeout} {mgmt_ip}"
        stdin, stdout, stderr = ssh_client.exec_command(cmd, timeout=timeout + 5)
        exit_status = stdout.channel.recv_exit_status()
        return exit_status == 0
    except Exception:
        return False


def _read_channel_until(channel, timeout_sec=1, stop_patterns=None):
    """Read from invoke_shell channel until timeout or any stop pattern is seen."""
    stop_patterns = stop_patterns or []
    buf = b""
    deadline = time.time() + timeout_sec
    try:
        while time.time() < deadline:
            if channel.recv_ready():
                try:
                    chunk = channel.recv(4096)
                except Exception:
                    break
                if not chunk:
                    break
                buf += chunk
                s = buf.decode(errors="ignore")
                for pat in stop_patterns:
                    if isinstance(pat, bytes):
                        if pat in buf:
                            return s
                    else:
                        if pat in s:
                            return s
            else:
                time.sleep(0.01)
    except Exception:
        pass
    return buf.decode(errors="ignore")


def telnet_and_run_show_serviceport(ssh_client, console_ip, device_port, login_user=SWITCH_USER, login_pass=SWITCH_PASSWORD):
    """
    Telnet to console_ip:device_port through the singleton ssh_client. If we reach a
    switch CLI prompt, run 'show serviceport' and return:
        {"interface_status": "up"/"down"/"busy"/None, "ip": "x.x.x.x"/None, "raw": "<output>"}
    On hard failure returns None.
    """
    try:
        if config.FLASK_DEBUG:
            print(f"[DEBUG] telnet_and_run_show_serviceport: console_ip={console_ip}, device_port={device_port}")

        if not device_port:
            if config.FLASK_DEBUG:
                print("[DEBUG] device_port is empty, aborting telnet attempt.")
            return False
        chan = ssh_client.invoke_shell()
        time.sleep(0.1)
        if config.FLASK_DEBUG:
            print("[DEBUG] Starting telnet session...")
        chan.send(f"telnet {console_ip} {device_port}\n")
        out = _read_channel_until(chan, timeout_sec=1)
        if config.FLASK_DEBUG:
            print(f"[DEBUG] After telnet command:\n{out!r}")
        chan.send("\n\n")
        out += _read_channel_until(chan, timeout_sec=.5)
        if config.FLASK_DEBUG:
            print(f"[DEBUG] After sending 2-newlines:\n{out!r}")

        if re.search(r"User[: ]*$", out, re.IGNORECASE):
            if config.FLASK_DEBUG:
                print("[DEBUG] User prompt detected, sending login_user")
            chan.send(login_user + "\n")
            out += _read_channel_until(chan, timeout_sec=1)
            if config.FLASK_DEBUG:
                print(f"[DEBUG] After sending login_user:\n{out!r}")
        if re.search(r"Password[: ]*$", out, re.IGNORECASE):
            if config.FLASK_DEBUG:
                print("[DEBUG] Password prompt detected, sending login_pass")
            chan.send(login_pass + "\n")
            out += _read_channel_until(chan, timeout_sec=1)
            if config.FLASK_DEBUG:
                print(f"[DEBUG] After sending login_pass:\n{out!r}")

        out += _read_channel_until(chan, timeout_sec=.5)
        if config.FLASK_DEBUG:
            print(f"[DEBUG] After waiting for CLI prompt:\n{out!r}")

        int_status = None
        if not re.search(r"\)\s?[>#]\s*$", out):
            int_status = "busy"
            if re.search(r"hunt group busy", out, re.IGNORECASE) or re.search(r"Connection refused", out, re.IGNORECASE):
                if config.FLASK_DEBUG:
                    print("[DEBUG] Detected 'hunt group busy' after telnet connection.")
                try:
                    chan.close()
                except Exception:
                    if config.FLASK_DEBUG:
                        print("[DEBUG] Exception during channel close after busy detection.")
                return {"interface_status": int_status, "ip": None, "raw": out}

            if re.search(r"Connected", out, re.IGNORECASE) and config.FLASK_DEBUG:
                print("[DEBUG] Detected Connection Success, sending newline")
            chan.send("\n")
            out += _read_channel_until(chan, timeout_sec=.5)
            if config.FLASK_DEBUG:
                print(f"[DEBUG] After second newline:\n{out!r}")

        if re.search(r"\)\s?[>#]\s*$", out):
            if config.FLASK_DEBUG:
                print("[DEBUG] CLI prompt detected, sending 'show serviceport'")
            chan.send("show serviceport\n")
            out += _read_channel_until(chan, timeout_sec=1)
            if config.FLASK_DEBUG:
                print(f"[DEBUG] Output after 'show serviceport':\n{out!r}")
            ip_addr = None
            m = re.search(r"Interface Status[\s\.\-:]*\s*(Up|Down|up|down)", out, re.IGNORECASE)
            if m:
                int_status = m.group(1).strip().lower()
                if config.FLASK_DEBUG:
                    print(f"[DEBUG] Parsed Interface Status: {int_status}")
            m2 = re.search(r"IP Address[\s\.\-:]*\s*([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)", out)
            if m2:
                ip_addr = m2.group(1).strip()
                if config.FLASK_DEBUG:
                    print(f"[DEBUG] Parsed IP Address: {ip_addr}")
            try:
                if config.FLASK_DEBUG:
                    print("[DEBUG] Closing telnet session gracefully...")
                chan.send("\x1d")
                chan.send("quit\n")
                _read_channel_until(chan, timeout_sec=.5)
            except Exception:
                if config.FLASK_DEBUG:
                    print("[DEBUG] Exception during telnet close.")
            try:
                chan.close()
            except Exception:
                if config.FLASK_DEBUG:
                    print("[DEBUG] Exception during channel close.")

            if config.FLASK_DEBUG:
                print(f"[DEBUG] Final log after conn-termination:\n{out!r}")
            return {"interface_status": int_status, "ip": ip_addr, "raw": out}
        else:
            if config.FLASK_DEBUG:
                print("[DEBUG] Could not reach CLI prompt, attempting to quit telnet session.")
            try:
                chan.send("\x1d")
                chan.send("quit\n")
            except Exception:
                if config.FLASK_DEBUG:
                    print("[DEBUG] Exception during telnet quit.")
            try:
                chan.close()
            except Exception:
                if config.FLASK_DEBUG:
                    print("[DEBUG] Exception during channel close.")

            if config.FLASK_DEBUG:
                print(f"[DEBUG] Final log after conn-termination:\n{out!r}")
            return None
    except Exception:
        traceback.print_exc()
        if config.FLASK_DEBUG:
            print("[DEBUG] Exception in telnet_and_run_show_serviceport.")
        return None


def ssh_and_set_hostname(ssh_client, mgmt_ip, new_hostname, serial_no='', login_user=SWITCH_USER, login_pass=SWITCH_PASSWORD):
    """
    Use the existing ssh_client (connected to PI/console) and from that shell run
    ``ssh login_user@mgmt_ip``, then ``en; hostname new_hostname; logout``.
    Returns ``(ok: bool, msg: str)``.
    """
    chan = None
    try:
        if config.FLASK_DEBUG:
            print(f"[DEBUG] ssh_and_set_hostname: mgmt_ip={mgmt_ip}, new_hostname={new_hostname}")

        if not mgmt_ip:
            if config.FLASK_DEBUG:
                print("[DEBUG] mgmt_ip is empty, aborting ssh attempt.")
            return False, "mgmt_ip not found!"

        chan = ssh_client.invoke_shell()
        time.sleep(0.1)

        chan.send(f"ssh {login_user}@{mgmt_ip}\n")
        out = _read_channel_until(chan, timeout_sec=1)

        if re.search(r"are you sure you want to continue connecting \(yes/no\)\s*$", out, re.IGNORECASE | re.MULTILINE):
            if config.FLASK_DEBUG:
                print("[DEBUG] Hostkey prompt detected, sending 'yes'")
            chan.send("yes\n")
            out += _read_channel_until(chan, timeout_sec=.5)

        if re.search(r"password[: ]*$", out, re.IGNORECASE | re.MULTILINE):
            if config.FLASK_DEBUG:
                print("[DEBUG] Password prompt detected, sending password")
            chan.send(login_pass + "\n")
            out += _read_channel_until(chan, timeout_sec=1)

        out += _read_channel_until(chan, timeout_sec=.5)
        if config.FLASK_DEBUG:
            print(f"[DEBUG] After SSH login attempt -> channel output:\n{out!r}")

        if re.search(r"permission denied", out, re.IGNORECASE):
            if config.FLASK_DEBUG:
                print("[DEBUG] SSH permission denied.")
            chan.send("exit\n")
            chan.close()
            return False, "SSH permission denied"
        if re.search(r"connection closed", out, re.IGNORECASE) or re.search(r"connection refused", out, re.IGNORECASE):
            if config.FLASK_DEBUG:
                print("[DEBUG] SSH connection failed/closed.")
            chan.close()
            return False, "SSH connection refused"

        if not re.search(r"\)[>#]\s*$", out):
            chan.send("\n")
            out += _read_channel_until(chan, timeout_sec=1)
            if config.FLASK_DEBUG:
                print(f"[DEBUG] After newline to elicit prompt:\n{out!r}")

        if re.search(r"\)[>#]\s*$", out):
            if config.FLASK_DEBUG:
                print("[DEBUG] Remote CLI prompt found. Entering enable (en) and setting hostname.")
            chan.send("en\n")
            out += _read_channel_until(chan, timeout_sec=1)
            if config.FLASK_DEBUG:
                print(f"[DEBUG] After 'en':\n{out!r}")

            chan.send("show version\n")
            out += _read_channel_until(chan, timeout_sec=1)
            if config.FLASK_DEBUG:
                print(f"[DEBUG] Output after 'show version':\n{out!r}")

            serial_match = re.search(r"Serial Number[\s\.\-:]*\s*([A-Za-z0-9]+)", out, re.IGNORECASE)
            parsed_serial = serial_match.group(1).strip() if serial_match else None

            if config.FLASK_DEBUG:
                print(f"[DEBUG] Parsed Serial Number: {parsed_serial} & Expected Serial Number: {serial_no}")

            if serial_no and parsed_serial:
                if parsed_serial.upper() != serial_no.upper():
                    if config.FLASK_DEBUG:
                        print(f"[DEBUG] Serial number mismatch! Expected: {serial_no}, Got: {parsed_serial}")
                    chan.send("logout\n")
                    out += _read_channel_until(chan, timeout_sec=1)
                    chan.close()
                    return False, f"Serial no. mismatch. Expected: {serial_no}, Got: {parsed_serial}"
                else:
                    if config.FLASK_DEBUG:
                        print(f"[DEBUG] Serial number verified successfully: {parsed_serial}")
            elif serial_no and not parsed_serial:
                if config.FLASK_DEBUG:
                    print("[DEBUG] Could not parse serial number from device output")

            chan.send(f"hostname {new_hostname}\n")
            out += _read_channel_until(chan, timeout_sec=1)
            if config.FLASK_DEBUG:
                print(f"[DEBUG] After 'hostname' cmd:\n{out!r}")

            out += _read_channel_until(chan, timeout_sec=.5)
            if config.FLASK_DEBUG:
                print(f"[DEBUG] After waiting for prompt change:\n{out!r}")

            prompt_patterns = [rf"\(\s*{re.escape(new_hostname)}\s*\)#\s*$", rf"\(\s*{re.escape(new_hostname)}\s*\)>\s*$"]
            prompt_ok = any(re.search(pat, out, re.MULTILINE) for pat in prompt_patterns)
            if config.FLASK_DEBUG:
                print(f"[DEBUG] Prompt detection patterns matched: {prompt_ok}")

            try:
                chan.send("logout\n")
                out += _read_channel_until(chan, timeout_sec=1)
                if config.FLASK_DEBUG:
                    print(f"[DEBUG] After sending logout:\n{out!r}")
                out += _read_channel_until(chan, timeout_sec=.5)
            except Exception as e:
                if config.FLASK_DEBUG:
                    print(f"[DEBUG] Exception while sending logout: {e}")

            if re.search(r"\[?[yY]/[nN]\]?\s*$", out, re.MULTILINE):
                if config.FLASK_DEBUG:
                    print("[DEBUG] Confirmation prompt detected after logout, sending 'y'")
                chan.send("y\n")
                out += _read_channel_until(chan, timeout_sec=.5)

            chan.close()
            return bool(prompt_ok), "Set hostname successful!"

        else:
            if config.FLASK_DEBUG:
                print("[DEBUG] No remote CLI prompt after SSH attempt; cleaning up.")
            chan.send("logout\n")
            out += _read_channel_until(chan, timeout_sec=.5)
            chan.close()
            return False, "Couldn't connect to device's CLI"

    except Exception as e:
        if config.FLASK_DEBUG:
            print(f"[DEBUG] Exception in ssh_and_set_hostname: {e}")
            traceback.print_exc()
        try:
            if chan is not None:
                chan.close()
        except Exception:
            pass
        return False, f"Exception: {e}"


# -------------------------------------------------------------------------------------------------
# Reservation Monitor (auto-release)
# -------------------------------------------------------------------------------------------------

class ReservationMonitor(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.keep_running = True

    def run(self):
        log_app("INFO", "[INFO] ReservationMonitor thread started", extra={"component": "reservation_monitor"})
        while self.keep_running:
            try:
                devices = read_devices()
                now = datetime.datetime.utcnow()

                for d in devices:
                    end_str = d.get("resv_end_time") or ""
                    if not end_str:
                        continue
                    try:
                        end_dt = datetime.datetime.fromisoformat(end_str)
                    except Exception:
                        continue
                    if now < end_dt:
                        continue

                    did = d["device_id"]
                    old_tag = d.get("tag") or "resv"
                    resv_user = (d.get("current_user") or "").strip() or "system"
                    print(f">>> $ [Auto-Release] Expired reservation for {did}. Releasing...")
                    log_app(
                        "INFO",
                        f"[INFO] auto-release: reservation expired for device_id={did}",
                        device_id=did, user=resv_user,
                        extra={
                            "component": "reservation_monitor",
                            "subsystem": "reservation",
                            "resv_end_time": end_str,
                        },
                    )

                    model_name = (d.get("model_name") or "").strip()
                    mgmt = d.get("mgmt_ip") or ""

                    update_resv(did, tag="free", current_user="", duration="", resv_end_time="")
                    log_op(
                        "EDIT", did,
                        {"field": "tag", "old": old_tag, "new": "free", "source": "auto-release"},
                        user=resv_user,
                    )

                    if mgmt and model_name:
                        if health_manager.ensure_ssh():
                            with health_manager.ssh_lock:
                                try:
                                    ok, msg = ssh_and_set_hostname(health_manager.ssh_client, mgmt, model_name)
                                    if ok:
                                        print(f">>> $ Hostname restored to {model_name} for device {did} (auto-release).")
                                        log_app(
                                            "INFO", f"[INFO] auto-release: hostname restored to {model_name!r}",
                                            device_id=did, user=resv_user,
                                            extra={
                                                "component": "reservation_monitor",
                                                "subsystem": "reservation",
                                                "mgmt_ip": mgmt,
                                            },
                                        )
                                    else:
                                        print(f">>> $ Hostname restore to {model_name} failed for device {did} (auto-release). Reason: {msg}")
                                        log_app(
                                            "WARNING", f"[WARNING] auto-release: hostname restore failed: {msg}",
                                            device_id=did, user=resv_user,
                                            extra={
                                                "component": "reservation_monitor",
                                                "subsystem": "reservation",
                                                "model_name": model_name,
                                                "mgmt_ip": mgmt,
                                            },
                                        )
                                except Exception as e:
                                    traceback.print_exc()
                                    log_app(
                                        "ERROR", f"[ERROR] auto-release: hostname restore exception: {e}",
                                        device_id=did, user=resv_user,
                                        extra={"component": "reservation_monitor", "subsystem": "reservation"},
                                    )
                        else:
                            print(f">>> $ Could not open SSH to LAB to restore hostname for device {did}.")
                            log_app(
                                "WARNING", "[WARNING] auto-release: SSH to lab unavailable; hostname not restored",
                                device_id=did, user=resv_user,
                                extra={"component": "reservation_monitor", "subsystem": "reservation"},
                            )
                    else:
                        log_app(
                            "WARNING", "[WARNING] auto-release: skipped hostname restore (missing mgmt_ip or model_name)",
                            device_id=did, user=resv_user,
                            extra={
                                "component": "reservation_monitor",
                                "subsystem": "reservation",
                                "has_mgmt": bool(mgmt),
                                "has_model": bool(model_name),
                            },
                        )

            except Exception as e:
                traceback.print_exc()
                log_app(
                    "ERROR",
                    f"[ERROR] ReservationMonitor loop exception: {e}",
                    extra={"component": "reservation_monitor"},
                )
            time.sleep(5)


reservation_monitor = ReservationMonitor()
