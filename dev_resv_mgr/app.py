import time, datetime
import os, threading, csv, json
import ipaddress, socket, subprocess
from filelock import FileLock
from flask import Flask, jsonify, request, render_template, send_from_directory, g
import paramiko, re
import traceback, argparse

# -----------------------
# Global Config (change accordingly)
# -----------------------
PI_IP = "10.25.4.200"
PI_USER = "host1"
PI_PASS = "sheldon123"
NUKE_IP = "10.25.4.201"
NUKE_USER = "swnuc01"
NUKE_PASS = "switch@123"
OfficeIP = NUKE_IP
SSH_USER = NUKE_USER
SSH_PASSWORD = NUKE_PASS

CONSOLE_IP = "192.168.1.102"
PORT_OFFSET = 10000
SWITCH_USER = "admin"
SWITCH_PASSWORD = "Netgear@@123"

AV_UI = {"port": 4443, "offset": 60000}
MAIN_UI = {"port": 49152, "offset": 51000}
MAIN_UI_OLD = {"port": 49151, "offset": 50000}

UP_HEALTH_TIMER = 10        # seconds
DOWN_HEALTH_TIMER = 2       # seconds
MAX_HEALTH_CHECK_RETRIES = 3

CSV_PATH = "database.csv"
CSV_LOCK_PATH = CSV_PATH + ".lock"
LOG_PATH = "operations.log"
LOG_LOCK_PATH = LOG_PATH + ".lock"
UPDATABLE_FIELDS = ["device_id", "serial_no", "model_name", "hw_id", "console_ip", "port_id"] 

# -----------------------
# Backend state & Operation Logging (Audit Trail)
# -------------------------------------------------------------------------------------------------
device_state = {}
device_state_lock = threading.Lock()

def is_valid_ipv4(addr):
    try:
        ipaddress.IPv4Address(addr)
        return True
    except Exception:
        return False

def read_devices_from_csv():
    devices = []
    lock = FileLock(CSV_LOCK_PATH, timeout=10)
    with lock:
        with open(CSV_PATH, newline='') as f:
            reader = csv.DictReader(f)
            for r in reader:
                if not r.get("tag"): r["tag"] = "free"
                devices.append(r)
    return devices

def write_devices_to_csv(devices):
    lock = FileLock(CSV_LOCK_PATH, timeout=10)
    with lock:
        with open(CSV_PATH, "w", newline='') as f:
            fieldnames = ["device_id","serial_no", "model_name","hw_id","mgmt_ip","console_ip","port_id","tag","current_user","duration","resv_end_time"]
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for d in devices:
                if not d.get("tag"):
                    d["tag"] = "free"
                writer.writerow(d)

def find_device(devices, device_id):
    for d in devices:
        if d["device_id"] == device_id: return d
    return None

def log_operation(operation, device_id, changes, user="system"):
    """
    Log all database operations for audit/revert capability.
    operation: ADD, EDIT, DELETE, or LOG (structured app/audit messages).
    Format: JSON lines with timestamp, operation, device_id, changes, user.
    """
    lock = FileLock(LOG_LOCK_PATH, timeout=10)
    with lock:
        timestamp = datetime.datetime.utcnow().isoformat()
        log_entry = {
            "timestamp": timestamp,
            "operation": operation,
            "device_id": device_id,
            "changes": changes,
            "user": user
        }
        with open(LOG_PATH, "a", newline='') as f:
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
        if u: return u
    return "system"


# -----------------------
# Health manager (single SSH session -> sequential pings)
# -------------------------------------------------------------------------------------------------

def init_device_state_from_csv():
    devices = read_devices_from_csv()
    with device_state_lock:
        for d in devices:
            did = d["device_id"]
            if did not in device_state:
                device_state[did] = {
                    "health": "unk", "retry_count": 0,
                    "next_check_ts": time.time() + 1
                }
    log_application("INFO", "[INFO] device_state initialized from CSV", extra={"device_count": len(devices)})

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
                log_application(
                    "INFO", f"[INFO] HealthManager SSH connected to {self.officeIP}",
                    extra={"component": "health", "host": self.officeIP},
                )
                return True
            except Exception as e:
                self.ssh_client = None
                log_application(
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
            log_application(
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

        log_application(
            "INFO",f"[INFO] health check cycle: pinging {len(check_list)} device(s)",
            extra={"component": "health", "checks": [{"device_id": did, "mgmt_ip": mip} for did, mip in check_list]},
        )
        with self.ssh_lock:
            client = self.ssh_client
            for device_id, mgmt_ip in check_list:
                try:
                    ok = ssh_ping_once(client, mgmt_ip)
                except Exception as e:
                    ok = False
                    log_application(
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
                        log_application(
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
                        log_application(
                            lvl, f"[{lvl}] health result DOWN mgmt={mgmt_ip} retry_count={st['retry_count']}",
                            device_id=device_id, extra={"component": "health", "mgmt_ip": mgmt_ip, "retry_count": st["retry_count"]},
                        )

    def run(self):
        log_application("INFO", "[INFO] HealthManager thread started", extra={"component": "health"})
        while self.keep_running:
            try:
                now = time.time()
                check_list_ids = []
                with device_state_lock:
                    for did, st in device_state.items():
                        if st["next_check_ts"] <= now:
                            check_list_ids.append(did)
                if check_list_ids:
                    csv_devices = read_devices_from_csv()
                    mgmt_map = {d["device_id"]: d["mgmt_ip"] for d in csv_devices}
                    to_ping = []
                    for did in check_list_ids:
                        mgmt = mgmt_map.get(did)
                        # skip devices without a valid mgmt_ip (health stays 'unk')
                        if mgmt and is_valid_ipv4(mgmt):
                            to_ping.append((did, mgmt))
                        else:
                            # set unknown if missing/invalid mgmt_ip
                            with device_state_lock:
                                st = device_state.get(did)
                                if st:
                                    st["health"] = "unk"
                                    st["retry_count"] = 0
                                    st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
                            log_application(
                                "INFO", f"[INFO] health skip (no valid mgmt_ip); state set unk",
                                device_id=did, extra={"component": "health", "mgmt_ip": mgmt},
                            )
                    self.run_one_cycle(to_ping)
                else:
                    time.sleep(0.5) # sleep half sec when check list is empty
            except Exception as e:
                traceback.print_exc()
                log_application(
                    "ERROR", f"[ERROR] HealthManager.run loop exception: {e}",
                    extra={"component": "health"},
                )
                self.close_ssh()
                time.sleep(self.reconnect_backoff)

    def stop(self):
        self.keep_running = False
        self.close_ssh()

health_manager = HealthManager(OfficeIP, SSH_USER, SSH_PASSWORD)

# -----------------------
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
    """
    Read from an invoke_shell channel until timeout or until any of stop_patterns
    (list of bytes or decoded strings) are seen. Returns the accumulated decoded text.
    Non-blocking: polls channel.recv_ready().
    """
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
                        if pat in buf: return s
                    else:
                        if pat in s: return s
            else:
                time.sleep(0.01)
    except Exception:
        pass
    return buf.decode(errors="ignore")

def telnet_and_run_show_serviceport(ssh_client, console_ip, device_port, login_user=SWITCH_USER, login_pass=SWITCH_PASSWORD):
    """
    Telnet to console_ip:device_port using an interactive shell (invoke_shell).
    Handles optional User:/Password: prompts, attempts to reach switch CLI prompt '>' or '#'.
    If it reaches prompt, runs 'show serviceport' and returns parsed result dict:
      {"interface_status": "Up"/"Down"/None, "ip": "x.x.x.x"/None, "raw": "<output>"}
    On failure returns None.
    """
    try:
        if app.debug:
            print(f"[DEBUG] telnet_and_run_show_serviceport: console_ip={console_ip}, device_port={device_port}")

        if not device_port:
            if app.debug:
                print("[DEBUG] device_port is empty, aborting telnet attempt.")
            return False
        chan = ssh_client.invoke_shell()
        time.sleep(0.1)
        if app.debug:
            print("[DEBUG] Starting telnet session...")
        # Start telnet
        chan.send(f"telnet {console_ip} {device_port}\n")
        # initial small read
        out = _read_channel_until(chan, timeout_sec=1)
        if app.debug:
            print(f"[DEBUG] After telnet command:\n{out!r}")
        # press enter (sometimes telnet waits)
        chan.send("\n\n")
        out += _read_channel_until(chan, timeout_sec=.5)
        if app.debug:
            print(f"[DEBUG] After sending 2-newlines:\n{out!r}")

        # handle login prompts (a couple iterations, some devices won't prompt)
        if re.search(r"User[: ]*$", out, re.IGNORECASE):
            if app.debug:
                print("[DEBUG] User prompt detected, sending login_user")
            chan.send(login_user + "\n")
            out += _read_channel_until(chan, timeout_sec=1)
            if app.debug:
                print(f"[DEBUG] After sending login_user:\n{out!r}")
        if re.search(r"Password[: ]*$", out, re.IGNORECASE):
            if app.debug:
                print("[DEBUG] Password prompt detected, sending login_pass")
            chan.send(login_pass + "\n")
            out += _read_channel_until(chan, timeout_sec=1)
            if app.debug:
                print(f"[DEBUG] After sending login_pass:\n{out!r}")

        # Now look for a CLI prompt '>' or '#'
        # Give it a few seconds to settle
        out += _read_channel_until(chan, timeout_sec=.5)
        if app.debug:
            print(f"[DEBUG] After waiting for CLI prompt:\n{out!r}")

        int_status = None
        # If we did not get a prompt yet, try sending a newline to elicit it
        if not re.search(r"\)\s?[>#]\s*$", out):
            int_status = "busy"
            # handle errors after telnet connection
            if re.search(r"hunt group busy", out, re.IGNORECASE) or re.search(r"Connection refused", out, re.IGNORECASE):
                if app.debug:
                    print("[DEBUG] Detected 'hunt group busy' after telnet connection.")
                try:
                    chan.close()
                except Exception:
                    if app.debug:
                        print("[DEBUG] Exception during channel close after busy detection.")
                    pass
                return {"interface_status": int_status, "ip": None, "raw": out}
            
            if re.search(r"Connected", out, re.IGNORECASE) and app.debug:
                print("[DEBUG] Detected Connection Success, sending newline")
            chan.send("\n")
            out += _read_channel_until(chan, timeout_sec=.5)
            if app.debug:
                print(f"[DEBUG] After second newline:\n{out!r}")

        # If we get a prompt, send the show command
        if re.search(r"\)\s?[>#]\s*$", out):
            if app.debug:
                print("[DEBUG] CLI prompt detected, sending 'show serviceport'")
            chan.send("show serviceport\n")
            # read the command output for a few seconds
            out += _read_channel_until(chan, timeout_sec=1)
            if app.debug:
                print(f"[DEBUG] Output after 'show serviceport':\n{out!r}")
            # Parse Interface Status and IP Address
            ip_addr = None
            # Try to find lines like: "Interface Status............................... Up"
            m = re.search(r"Interface Status[\s\.\-:]*\s*(Up|Down|up|down)", out, re.IGNORECASE)
            if m:
                int_status = m.group(1).strip().lower()
                if app.debug:
                    print(f"[DEBUG] Parsed Interface Status: {int_status}")
            # Try to find "IP Address........ 192.168.1.187"
            m2 = re.search(r"IP Address[\s\.\-:]*\s*([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)", out)
            if m2:
                ip_addr = m2.group(1).strip()
                if app.debug:
                    print(f"[DEBUG] Parsed IP Address: {ip_addr}")
            # close telnet gracefully
            try:
                if app.debug:
                    print("[DEBUG] Closing telnet session gracefully...")
                # send escape (ctrl+]) then quit
                chan.send("\x1d")
                chan.send("quit\n")
                _read_channel_until(chan, timeout_sec=.5)
            except Exception:
                if app.debug:
                    print("[DEBUG] Exception during telnet close.")
                pass
            try:
                chan.close()
            except Exception:
                if app.debug:
                    print("[DEBUG] Exception during channel close.")
                pass

            if app.debug:
                print(f"[DEBUG] Final log after conn-termination:\n{out!r}")
            return {"interface_status": int_status, "ip": ip_addr, "raw": out}
        else:
            # Couldn't reach a prompt; try to quit and return None
            if app.debug:
                print("[DEBUG] Could not reach CLI prompt, attempting to quit telnet session.")
            try:
                chan.send("\x1d")
                chan.send("quit\n")
            except Exception:
                if app.debug:
                    print("[DEBUG] Exception during telnet quit.")
                pass
            try:
                chan.close()
            except Exception:
                if app.debug:
                    print("[DEBUG] Exception during channel close.")
                pass
            
            if app.debug:
                print(f"[DEBUG] Final log after conn-termination:\n{out!r}")
            return None
    except Exception:
        traceback.print_exc()
        if app.debug:
            print("[DEBUG] Exception in telnet_and_run_show_serviceport.")
        return None

# CHANGE: replace telnet-based hostname setter with nested SSH over the PI (uses existing ssh_client.invoke_shell())
def ssh_and_set_hostname(ssh_client, mgmt_ip, new_hostname, serial_no='', login_user=SWITCH_USER, login_pass=SWITCH_PASSWORD):
    """
    Use the existing ssh_client (connected to the PI/console) and from that shell run:
        ssh {login_user}@{mgmt_ip}
    Then perform:
        en; hostname {new_hostname}; logout
    Returns True if the hostname change is detected (prompt contains new_hostname# or (new_hostname)#), False otherwise.
    """
    try:
        if app.debug:
            print(f"[DEBUG] ssh_and_set_hostname: mgmt_ip={mgmt_ip}, new_hostname={new_hostname}")

        if not mgmt_ip:
            if app.debug:
                print("[DEBUG] mgmt_ip is empty, aborting ssh attempt.")
            return False, "mgmt_ip not found!"

        chan = ssh_client.invoke_shell()
        time.sleep(0.1)

        # Start nested ssh
        chan.send(f"ssh {login_user}@{mgmt_ip}\n")
        out = _read_channel_until(chan, timeout_sec=1)

        # Handle first-time hostkey prompt: "Are you sure you want to continue connecting (yes/no)?"
        if re.search(r"are you sure you want to continue connecting \(yes/no\)\s*$", out, re.IGNORECASE | re.MULTILINE):
            if app.debug:
                print("[DEBUG] Hostkey prompt detected, sending 'yes'")
            chan.send("yes\n")
            out += _read_channel_until(chan, timeout_sec=.5)

        # Handle password prompt if present
        if re.search(r"password[: ]*$", out, re.IGNORECASE | re.MULTILINE):
            if app.debug:
                print("[DEBUG] Password prompt detected, sending password")
            chan.send(login_pass + "\n")
            out += _read_channel_until(chan, timeout_sec=1)

        # Quick read to settle into remote CLI
        out += _read_channel_until(chan, timeout_sec=.5)
        if app.debug:
            print(f"[DEBUG] After SSH login attempt -> channel output:\n{out!r}")

        # Detect common failure messages
        if re.search(r"permission denied", out, re.IGNORECASE):
            if app.debug:
                print("[DEBUG] SSH permission denied.")
            chan.send("exit\n")
            chan.close()
            return False, "SSH permission denied"
        if re.search(r"connection closed", out, re.IGNORECASE) or re.search(r"connection refused", out, re.IGNORECASE):
            if app.debug:
                print("[DEBUG] SSH connection failed/closed.")
            chan.close()
            return False, "SSH connection refused"

        # Ensure we have a switch CLI prompt (ending with ')>' or ')#')
        if not re.search(r"\)[>#]\s*$", out):
            # try nudging with newline
            chan.send("\n")
            out += _read_channel_until(chan, timeout_sec=1)
            if app.debug:
                print(f"[DEBUG] After newline to elicit prompt:\n{out!r}")

        # If we see a CLI prompt, proceed
        if re.search(r"\)[>#]\s*$", out):
            if app.debug:
                print("[DEBUG] Remote CLI prompt found. Entering enable (en) and setting hostname.")
            # Enter enable mode
            chan.send("en\n")
            out += _read_channel_until(chan, timeout_sec=1)
            if app.debug:
                print(f"[DEBUG] After 'en':\n{out!r}")

            # --- verify if serial number matches with the device ---
            chan.send("show version\n")
            out += _read_channel_until(chan, timeout_sec=1)
            if app.debug:
                print(f"[DEBUG] Output after 'show version':\n{out!r}")
            
            # Parse Serial Number from output
            serial_match = re.search(r"Serial Number[\s\.\-:]*\s*([A-Za-z0-9]+)", out, re.IGNORECASE)
            parsed_serial = serial_match.group(1).strip() if serial_match else None
            
            if app.debug:
                print(f"[DEBUG] Parsed Serial Number: {parsed_serial} & Expected Serial Number: {serial_no}")
            
            # Verify serial number if provided
            if serial_no and parsed_serial:
                if parsed_serial.upper() != serial_no.upper():
                    if app.debug:
                        print(f"[DEBUG] Serial number mismatch! Expected: {serial_no}, Got: {parsed_serial}")
                    chan.send("logout\n")
                    out += _read_channel_until(chan, timeout_sec=1)
                    chan.close()
                    return False, f"Serial no. mismatch. Expected: {serial_no}, Got: {parsed_serial}"
                else:
                    if app.debug:
                        print(f"[DEBUG] Serial number verified successfully: {parsed_serial}")
            elif serial_no and not parsed_serial:
                if app.debug:
                    print(f"[DEBUG] Could not parse serial number from device output")
                # Proceed anyway if we can't parse, but log warning
                pass

            # Send hostname command
            chan.send(f"hostname {new_hostname}\n")
            out += _read_channel_until(chan, timeout_sec=1)
            if app.debug:
                print(f"[DEBUG] After 'hostname' cmd:\n{out!r}")

            # Allow a short moment for prompt to change
            out += _read_channel_until(chan, timeout_sec=.5)
            if app.debug:
                print(f"[DEBUG] After waiting for prompt change:\n{out!r}")

            # Detect prompt like: (<new_hostname>)#  OR  (<new_hostname>)#
            prompt_patterns = [rf"\(\s*{re.escape(new_hostname)}\s*\)#\s*$", rf"\(\s*{re.escape(new_hostname)}\s*\)>\s*$"]
            prompt_ok = any(re.search(pat, out, re.MULTILINE) for pat in prompt_patterns)
            if app.debug:
                print(f"[DEBUG] Prompt detection patterns matched: {prompt_ok}")

            # Attempt to logout gracefully (switches commonly use 'logout' rather than 'exit')
            try:
                chan.send("logout\n")
                out += _read_channel_until(chan, timeout_sec=1)
                if app.debug:
                    print(f"[DEBUG] After sending logout:\n{out!r}")
                # Confirm logout closure
                out += _read_channel_until(chan, timeout_sec=.5)
            except Exception as e:
                if app.debug:
                    print(f"[DEBUG] Exception while sending logout: {e}")

            # If remote side asks for any confirmation like [y/n], answer 'y' (best-effort)
            if re.search(r"\[?[yY]/[nN]\]?\s*$", out, re.MULTILINE):
                if app.debug:
                    print("[DEBUG] Confirmation prompt detected after logout, sending 'y'")
                chan.send("y\n")
                out += _read_channel_until(chan, timeout_sec=.5)

            # Close channel and return result
            chan.close()
            return bool(prompt_ok), "Set hostname successful!"

        else:
            # Didn't reach remote CLI prompt -> try to clean up
            if app.debug:
                print("[DEBUG] No remote CLI prompt after SSH attempt; cleaning up.")
            
            chan.send("logout\n")
            out += _read_channel_until(chan, timeout_sec=.5)
            chan.close()
            return False, "Couldn't connect to device's CLI"

    except Exception as e:
        if app.debug:
            print(f"[DEBUG] Exception in ssh_and_set_hostname: {e}")
            traceback.print_exc()
        chan.close()
        return False, f"Exception: {e}"

# -----------------------
# Reservation Monitor (auto-release)
# -------------------------------------------------------------------------------------------------

class ReservationMonitor(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.keep_running = True

    def run(self):
        log_application("INFO", "[INFO] ReservationMonitor thread started", extra={"component": "reservation_monitor"})
        while self.keep_running:
            try:
                # 1. Read Database
                devices = read_devices_from_csv()
                dirty = False
                now = datetime.datetime.utcnow()

                # 2. Check for expirations
                for d in devices:
                    end_str = d.get("resv_end_time") or ""
                    if end_str:
                        end_dt = datetime.datetime.fromisoformat(end_str)
                        # Buffer of 1 second to ensure we don't race with the very last second of display
                        if now >= end_dt:
                            did = d["device_id"]
                            old_tag = d.get("tag") or "resv"
                            resv_user = (d.get("current_user") or "").strip() or "system"
                            print(f">>> $ [Auto-Release] Expired reservation for {did}. Releasing...")
                            log_application(
                                "INFO",
                                f"[INFO] auto-release: reservation expired for device_id={did}",
                                device_id=did,
                                user=resv_user,
                                extra={
                                    "component": "reservation_monitor",
                                    "subsystem": "reservation",
                                    "resv_end_time": end_str,
                                },
                            )

                            # Capture model_name
                            model_name = (d.get("model_name") or "").strip()
                            mgmt = d.get("mgmt_ip") or ""
                            
                            # Mark as released locally (CSV will be written later)
                            d["tag"] = "free"
                            d["current_user"] = ""
                            d["duration"] = ""
                            d["resv_end_time"] = ""
                            dirty = True
                            log_operation(
                                "EDIT", did,
                                {"field": "tag", "old": old_tag, "new": "free", "source": "auto-release"},
                                user=resv_user,
                            )

                            # Best-effort: restore hostname on physical switch back to model_name
                            # Use the singleton SSH client guarded by health_manager.ssh_lock
                            if mgmt and model_name:
                                if health_manager.ensure_ssh():
                                    with health_manager.ssh_lock:
                                        try:
                                            ok, msg = ssh_and_set_hostname(health_manager.ssh_client, mgmt, model_name)
                                            if ok:
                                                print(f">>> $ Hostname restored to {model_name} for device {did} (auto-release).")
                                                log_application(
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
                                                log_application(
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
                                            log_application(
                                                "ERROR", f"[ERROR] auto-release: hostname restore exception: {e}",
                                                device_id=did, user=resv_user,
                                                extra={"component": "reservation_monitor", "subsystem": "reservation"},
                                            )
                                else:
                                    print(f">>> $ Could not open SSH to LAB to restore hostname for device {did}.")
                                    log_application(
                                        "WARNING", "[WARNING] auto-release: SSH to lab unavailable; hostname not restored",
                                        device_id=did, user=resv_user,
                                        extra={"component": "reservation_monitor", "subsystem": "reservation"},
                                    )
                            elif not mgmt or not model_name:
                                log_application(
                                    "WARNING", "[WARNING] auto-release: skipped hostname restore (missing mgmt_ip or model_name)",
                                    device_id=did,
                                    user=resv_user,
                                    extra={
                                        "component": "reservation_monitor",
                                        "subsystem": "reservation",
                                        "has_mgmt": bool(mgmt),
                                        "has_model": bool(model_name),
                                    },
                                )
                
                # 3. Save if changes made
                if dirty:
                    write_devices_to_csv(devices)
                    log_application(
                        "INFO", "[INFO] auto-release: CSV updated after expirations",
                        user="system",
                        extra={"component": "reservation_monitor", "subsystem": "reservation"},
                    )
                
            except Exception as e:
                traceback.print_exc()
                log_application(
                    "ERROR",
                    f"[ERROR] ReservationMonitor loop exception: {e}",
                    extra={"component": "reservation_monitor"},
                )
            # Check every 5 seconds to reduce I/O load (Backoff on error)
            time.sleep(5) 

# Initialize the monitor
reservation_monitor = ReservationMonitor()

# -----------------------
# Flask app + API
# -------------------------------------------------------------------------------------------------
app = Flask(__name__, static_folder="static", template_folder="templates")
http_ignore_api_logs = ["/api/devices", "/api/config"]

@app.before_request
def _log_http_request_start():
    if request.path in http_ignore_api_logs: return
    g._req_started = time.time()
    log_application(
        "INFO", f"[INFO] {request.method}: {request.path}",
        extra={"component": "http", "http_method": request.method, "path": request.path, "phase": "request"},
    )


@app.after_request
def _log_http_request_end(response):
    if request.path in http_ignore_api_logs: return response
    elapsed_ms = None
    if getattr(g, "_req_started", None) is not None:
        elapsed_ms = round((time.time() - g._req_started) * 1000, 2)
    code = response.status_code if response is not None else 0
    if code >= 500:
        lvl = "ERROR"
    elif code >= 400:
        lvl = "WARNING"
    else:
        lvl = "INFO"
    extra = {
        "component": "http",
        "http_method": request.method,
        "path": request.path,
        "status_code": code,
        "phase": "response",
    }
    if elapsed_ms is not None:
        extra["elapsed_ms"] = elapsed_ms
    log_application(
        lvl,
        f"[{lvl}] {request.method}: {request.path} -> {code}",
        extra=extra,
    )
    return response


@app.route("/")
def index():
    #return render_template("index.html") # version-1
    return render_template("minimal.html") # version-2

@app.route('/static/<path:p>')
def static_files(p):
    return send_from_directory('static', p)

def format_reservation_block(d):
    """
    Returns the reservation block string exactly in the desired format (keeps newline).
    If tag == resv: full block with User, Duration hh/mm, Time Left, Start, End.
    If tag == static: show owner only.
    If tag == free: empty string (frontend will show inputs).
    """
    tag = (d.get("tag") or "").lower()
    if tag == "resv":
        current_user = d.get("current_user") or "-"
        # compute hh/mm from duration
        dur = d.get("duration") or ""
        try:
            dur_min = int(dur)
        except Exception:
            dur_min = 0
        hh = dur_min // 60
        mm = dur_min % 60
        resv_end = d.get("resv_end_time") or ""
        try:
            end_dt = datetime.datetime.fromisoformat(resv_end) if resv_end else None
        except Exception:
            end_dt = None
        now = datetime.datetime.utcnow()
        time_left = ""
        if end_dt:
            delta = end_dt - now
            if delta.total_seconds() > 0:
                th = int(delta.total_seconds() // 3600)
                tm = int((delta.total_seconds() % 3600) // 60)
                ts = int(delta.total_seconds() % 60)
                time_left = f"{th:02d}:{tm:02d}:{ts:02d}"
            else:
                time_left = "00:00:00"
            time_format = "%d-%m-%y %I:%M.%p"
            start_dt = end_dt - datetime.timedelta(minutes=dur_min) if dur_min else None
            start_str = start_dt.strftime(time_format) if start_dt else "-"
            end_str = end_dt.strftime(time_format)
        else:
            time_left = "-"
            start_str = "-"
            end_str = "-"
        # preserve requested format as closely as possible
        return f"User: {current_user}, Duration: {hh}hrs,{mm}mins\nStart: {start_str} End: {end_str}\nTime Left: {time_left}"
    elif tag == "static":
        owner = d.get("current_user") or "-"
        return f"Static Reservation Owner: {owner}"
    else:
        return ""

@app.route("/api/devices", methods=["GET"])
def api_devices():
    devices = read_devices_from_csv()
    #log_application("INFO", "[INFO] api_devices: building device list", extra={"component": "api", "count": len(devices)})
    now = datetime.datetime.utcnow()
    output = []
    with device_state_lock:
        for d in devices:
            did = d["device_id"]
            mgmt = d.get("mgmt_ip") or ""
            # if mgmt missing/invalid => health unknown
            if not mgmt or not is_valid_ipv4(mgmt):
                health = "unk"
                retry_count = 0
            else:
                st = device_state.get(did, {"health":"unk","retry_count":0,"next_check_ts":time.time()+1})
                health = st["health"]
                retry_count = st["retry_count"]
            resv_block = format_reservation_block(d)
            output.append({
                **d,
                "health": health,
                "retry_count": retry_count,
                "resv_block": resv_block
            })
    return jsonify({"devices": output})

@app.route("/api/config", methods=["POST"])
def api_config():
    data = request.get_json()
    mgmt_ip = data.get("mgmt_ip")
    if not mgmt_ip:
        log_application("WARNING", "[WARNING] api_config: mgmt_ip missing", extra={"component": "api"})
        return jsonify({"error":"mgmt_ip required"}), 400
    
    # Extract last octet (e.g., 192.168.1.55 -> 55)
    last_octet = int(mgmt_ip.strip().split(".")[-1])
    
    # Calculate ports based on global config offsets
    av_port = AV_UI["offset"] + last_octet
    old_main_port = MAIN_UI_OLD["offset"] + last_octet
    new_main_port = MAIN_UI["offset"] + last_octet

    out = {
        "switch_id": last_octet,
        "av_port": av_port,
        "new_main_port": new_main_port,
        "old_main_port": old_main_port,
        "port_offset": PORT_OFFSET,
    }
    #log_application("INFO", f"[INFO] api_config: computed ports for mgmt_ip={mgmt_ip}", extra={"component": "api", **out})
    return jsonify(out)

# -------------------------------------------------------------------------------------------------

@app.route("/api/remove_mgmt_ip", methods=["POST"])
def api_remove_mgmt_ip():
    body = request.get_json()
    device_id = body.get("device_id")
    if not device_id:
        log_application("WARNING", "[WARNING] api_remove_mgmt_ip: device_id missing", extra={"component": "api"})
        return jsonify({"ok": False, "error": "device_id required"}), 400
    devices = read_devices_from_csv()
    d = find_device(devices, device_id)
    if not d:
        log_application("WARNING", f"[WARNING] api_remove_mgmt_ip: device not found device_id={device_id}", device_id=device_id, extra={"component": "api"})
        return jsonify({"ok": False, "error": "device not found"}), 404
    old_ip = d.get("mgmt_ip", "")
    d["mgmt_ip"] = ""
    write_devices_to_csv(devices)
    log_operation("EDIT", device_id, {"field": "mgmt_ip", "old": old_ip, "new": ""})
    log_application("INFO", f"[INFO] api_remove_mgmt_ip: cleared mgmt_ip (was {old_ip!r})", device_id=device_id, extra={"component": "api", "old_mgmt_ip": old_ip})
    return jsonify({"ok": True})

@app.route("/api/reserve", methods=["POST"])
def api_reserve():
    body = request.get_json() or {}
    device_id = body.get("device_id")
    user = body.get("user")
    hours = int(body.get("hours", 0))
    minutes = int(body.get("minutes", 0))
    duration_minutes = hours * 60 + minutes
    if not device_id or not user:
        log_application(
            "WARNING",
            "[WARNING] api_reserve: device_id or user missing",
            user=reservation_actor_user(body),
            extra={"component": "api", "subsystem": "reservation"},
        )
        return jsonify({"error": "device_id and user required"}), 400
    devices = read_devices_from_csv()
    d = find_device(devices, device_id)
    if not d:
        log_application(
            "WARNING",
            f"[WARNING] api_reserve: device not found device_id={device_id}",
            device_id=device_id,
            user=user,
            extra={"component": "api", "subsystem": "reservation"},
        )
        return jsonify({"error": "device not found"}), 404
    log_application(
        "INFO",
        f"[INFO] api_reserve: attempt user={user!r} hours={hours} minutes={minutes}",
        device_id=device_id,
        user=user,
        extra={"component": "api", "subsystem": "reservation", "hours": hours, "minutes": minutes},
    )
    old_tag = d.get("tag", "free")
    # Static Reservation if >= 24 hours
    if hours >= 24:
        d["tag"] = "static"
        d["current_user"] = user
        d["duration"] = ""        # No duration for static
        d["resv_end_time"] = ""   # No end time for static
    else:
        duration_minutes = hours * 60 + minutes
        now = datetime.datetime.utcnow()
        end = now + datetime.timedelta(minutes=duration_minutes)
        d["tag"] = "resv"
        d["current_user"] = user
        d["duration"] = str(duration_minutes)
        d["resv_end_time"] = end.isoformat()

    # --- Attempt to update switch hostname via telnet over singleton ssh_client ---
    console_ip = d.get("console_ip") or ""
    if not console_ip:
        log_application(
            "WARNING",
            "[WARNING] api_reserve: console_ip missing",
            device_id=device_id,
            user=user,
            extra={"component": "api", "subsystem": "reservation"},
        )
        return jsonify({"error": "console_ip not found"}), 404
    port_id = int(d.get("port_id", 0))
    if not port_id:
        log_application(
            "WARNING",
            "[WARNING] api_reserve: port_id missing",
            device_id=device_id,
            user=user,
            extra={"component": "api", "subsystem": "reservation"},
        )
        return jsonify({"error": "port_id not found"}), 404

    ok = False
    msg = ''
    # try to set hostname to current user (trim all whitespace)
    new_name = device_id + '-' + re.sub(r"\s+", "", d.get("current_user") or "")
    mgmt = d.get("mgmt_ip") or ""
    serial_no = d.get("serial_no") or ""
    if not new_name or not mgmt:
        log_application(
            "WARNING", f"[WARNING] api_reserve: skip hostname (new_name={new_name!r} mgmt={mgmt!r})",
            device_id=device_id,
            user=user,
            extra={"component": "api", "subsystem": "reservation"},
        )
    elif not health_manager.ensure_ssh():
        log_application(
            "WARNING", "[WARNING] api_reserve: SSH unavailable; hostname not set",
            device_id=device_id, user=user,
            extra={"component": "api", "subsystem": "reservation"},
        )
    else:
        with health_manager.ssh_lock:
            try:
                ok, msg = ssh_and_set_hostname(health_manager.ssh_client, mgmt, new_name, serial_no)
            except Exception as e:
                traceback.print_exc()
                msg = str(e)
                log_application(
                    "ERROR", f"[ERROR] api_reserve: ssh_and_set_hostname exception: {e}",
                    device_id=device_id, ser=user,
                    extra={"component": "api", "subsystem": "reservation"},
                )
    
    if ok:
        print(f">>> $ Hostname set to {new_name} for device: {device_id}")
        write_devices_to_csv(devices)
        log_operation(
            "EDIT", device_id,
            {"field": "tag", "old": old_tag, "new": d["tag"], "reservation_user": user},
            user=user,
        )
        log_application(
            "INFO", f"[INFO] api_reserve: success tag={d['tag']!r} hostname={new_name!r}",
            device_id=device_id, user=user,
            extra={
                "component": "api",
                "subsystem": "reservation",
                "resv_end_time": d.get("resv_end_time"),
            },
        )
    else:
        print(f">>> $ Hostname change to {new_name} failed for device: {device_id}")
        log_application(
            "WARNING", f"[WARNING] api_reserve: failed (CSV not committed) msg={msg!r}",
            device_id=device_id,
            user=user,
            extra={"component": "api", "subsystem": "reservation", "new_hostname": new_name},
        )
    return jsonify({"ok": ok, "resv_end_time": d["resv_end_time"], "msg": msg})


@app.route("/api/release", methods=["POST"])
def api_release():
    body = request.get_json() or {}
    device_id = body.get("device_id")
    if not device_id:
        log_application(
            "WARNING", "[WARNING] api_release: device_id missing",
            user=reservation_actor_user(body),
            extra={"component": "api", "subsystem": "reservation"},
        )
        return jsonify({"error": "device_id required"}), 400
    devices = read_devices_from_csv()
    d = find_device(devices, device_id)
    if not d:
        log_application(
            "WARNING", f"[WARNING] api_release: device not found device_id={device_id}",
            device_id=device_id, user=reservation_actor_user(body),
            extra={"component": "api", "subsystem": "reservation"},
        )
        return jsonify({"error": "device not found"}), 404
    ru = reservation_actor_user(body, d)
    log_application(
        "INFO", "[INFO] api_release: attempt",
        device_id=device_id, user=ru,
        extra={
            "component": "api",
            "subsystem": "reservation",
            "current_user": d.get("current_user"),
        },
    )
    
    old_tag = d.get("tag", "free")
    old_user = d.get("current_user", "")
    # Save model_name before overwriting fields (we need it to restore hostname)
    model_name = (d.get("model_name") or "").strip()
    mgmt = d.get("mgmt_ip") or ""

    d["current_user"] = ""
    d["duration"] = ""
    d["resv_end_time"] = ""
    d["tag"] = "free"


    ok = False
    msg = ""
    hostname_attempted = False
    if not mgmt or not model_name:
        log_application(
            "WARNING", f"[WARNING] api_release: skip hostname restore (mgmt={bool(mgmt)} model_name={bool(model_name)})",
            device_id=device_id,
            user=ru,
            extra={"component": "api", "subsystem": "reservation"},
        )
    elif not health_manager.ensure_ssh():
        log_application(
            "WARNING",
            "[WARNING] api_release: SSH unavailable; hostname not restored",
            device_id=device_id,
            user=ru,
            extra={"component": "api", "subsystem": "reservation"},
        )
    else:
        hostname_attempted = True
        with health_manager.ssh_lock:
            try:
                ok, msg = ssh_and_set_hostname(health_manager.ssh_client, mgmt, model_name)
            except Exception as e:
                traceback.print_exc()
                msg = str(e)
                log_application(
                    "ERROR", f"[ERROR] api_release: ssh_and_set_hostname exception: {e}",
                    device_id=device_id, user=ru,
                    extra={"component": "api", "subsystem": "reservation"},
                )

    if ok:
        print(f">>> $ Hostname restored to {model_name} for device {device_id}")
        log_application(
            "INFO", f"[INFO] api_release: hostname restored to {model_name!r}",
            device_id=device_id, user=ru,
            extra={"component": "api", "subsystem": "reservation"},
        )
    elif hostname_attempted:
        print(f">>> $ Hostname restore to {model_name} failed for device {device_id}")
        log_application(
            "WARNING", f"[WARNING] api_release: hostname restore not ok msg={msg!r}",
            device_id=device_id, user=ru,
            extra={"component": "api", "subsystem": "reservation", "model_name": model_name},
        )
    write_devices_to_csv(devices) # update in database even if reset hostname fails
    log_operation(
        "EDIT", device_id,
        {
            "field": "tag",
            "old": old_tag,
            "new": "free",
            "released_user": old_user,
            "reservation_user": ru,
        },
        user=ru,
    )
    log_application(
        "INFO", "[INFO] api_release: CSV updated tag=free",
        device_id=device_id, user=ru,
        extra={"component": "api", "subsystem": "reservation", "released_user": old_user},
    )
    return jsonify({"ok": ok, "msg": msg})


@app.route("/api/refresh_health", methods=["POST"])
def api_refresh_health():
    body = request.get_json()
    device_id = body.get("device_id")
    if not device_id:
        log_application("WARNING", "[WARNING] api_refresh_health: device_id missing", extra={"component": "api"})
        return jsonify({"error": "device_id required"}), 400
    devices = read_devices_from_csv()
    d = find_device(devices, device_id)
    if not d:
        log_application("WARNING", f"[WARNING] api_refresh_health: device not found device_id={device_id}", device_id=device_id, extra={"component": "api"})
        return jsonify({"error": "device not found"}), 404
    log_application("INFO", "[INFO] api_refresh_health: request", device_id=device_id, extra={"component": "api"})

    # load CSV to find related metadata (port_id, mgmt_ip)
    console_ip = d.get("console_ip") or ""
    port_id = int(d.get("port_id", 0))
    mgmt_ip = d.get("mgmt_ip") or ""

    with device_state_lock:
        st = device_state.get(device_id)
        if not st:
            # initialize unknown state and schedule immediate handling
            device_state[device_id] = {
                "health": "unk",
                "retry_count": 0,
                "next_check_ts": time.time() + 0.1
            }
            log_application("INFO", "[INFO] api_refresh_health: initialized device_state", device_id=device_id, extra={"component": "api"})
            return jsonify({"ok": True})

        health = st.get("health", "unk").lower()
    
    # Branch by health -> if health == "up" or health == "down", its mgmt_ip should be present
    if mgmt_ip and is_valid_ipv4(mgmt_ip):
        # Try one immediate ping if we can (best-effort)
        if health_manager.ensure_ssh():
            with health_manager.ssh_lock:
                try:
                    ok = ssh_ping_once(health_manager.ssh_client, mgmt_ip, count=2, timeout=4)
                except Exception as e:
                    ok = False
                    log_application("ERROR", f"[ERROR] api_refresh_health: ping exception: {e}", device_id=device_id, extra={"component": "api", "mgmt_ip": mgmt_ip})
            with device_state_lock:
                if ok:
                    st["health"] = "up"
                    st["retry_count"] = 0
                    st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
                else:
                    # Mark down and start retry sequence
                    st["health"] = "down"
                    st["retry_count"] = 1
                    st["next_check_ts"] = time.time() + DOWN_HEALTH_TIMER
            log_application(
                "INFO" if ok else "WARNING",
                f"[{'INFO' if ok else 'WARNING'}] api_refresh_health: immediate ping mgmt={mgmt_ip} -> {'up' if ok else 'down'}",
                device_id=device_id, extra={"component": "api", "status": st["health"]},
            )
            return jsonify({"ok": True, "status": st["health"]})
        else:
            log_application("WARNING", "[WARNING] api_refresh_health: unable to open ssh to lab (ping path)", device_id=device_id, extra={"component": "api"})
            return jsonify({"ok": False,  "reason": "unable to open ssh to pi"})
    
    # if health == "unk" --> most likely mgmt_ip not present
    else: 
        # Attempt telnet to console to discover mgmt IP and update if Interface Status is Up
        if not (console_ip and is_valid_ipv4(console_ip)): # can't telnet without console ip info (ipv4)
            with device_state_lock:
                st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
            log_application("WARNING", "[WARNING] api_refresh_health: missing/invalid console_ip", device_id=device_id, extra={"component": "api"})
            return jsonify({"ok": False, "reason": "missing/invalid console_ip, need manual input"})
        
        if not port_id: # can't telnet without port info (non zero)
            # initialize state to re-check later
            with device_state_lock:
                st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
            log_application("WARNING", "[WARNING] api_refresh_health: missing port_id", device_id=device_id, extra={"component": "api"})
            return jsonify({"ok": False, "reason": "missing port_id, need manual input"})

        # Use health_manager's ssh client (singleton). Best-effort.
        if not health_manager.ensure_ssh():
            # schedule retry
            with device_state_lock:
                st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
            log_application("WARNING", "[WARNING] api_refresh_health: unable to open ssh to lab (telnet path)", device_id=device_id, extra={"component": "api"})
            return jsonify({"ok": False, "reason": "unable to open ssh to lab"})

        with health_manager.ssh_lock:
            try:
                res = telnet_and_run_show_serviceport(health_manager.ssh_client, console_ip, PORT_OFFSET + port_id)
            except Exception as e:
                res = None
                log_application("ERROR", f"[ERROR] api_refresh_health: telnet exception: {e}", device_id=device_id, extra={"component": "api"})

        if not res:
            # failed to get info — schedule re-check later
            with device_state_lock:
                st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
            log_application("WARNING", "[WARNING] api_refresh_health: telnet returned no data", device_id=device_id, extra={"component": "api"})
            return jsonify({"ok": False, "reason": "telnet did not return data"})

        # If Interface Status is up and IP found, update CSV mgmt_ip and schedule immediate ping
        int_status = (res.get("interface_status") or "").lower() if res.get("interface_status") else None
        ip_addr = res.get("ip")
        if int_status == "up" and ip_addr and is_valid_ipv4(ip_addr):
            # Update CSV (atomic via FileLock) and device_state to ping immediately
            if d:
                d["mgmt_ip"] = ip_addr
                write_devices_to_csv(devices)
                with device_state_lock:
                    st["health"] = "unk"   # keep as unk until ping completes
                    st["retry_count"] = 0
                    st["next_check_ts"] = time.time() + 0.1
                log_operation("EDIT", device_id, {"field": "mgmt_ip", "old": mgmt_ip, "new": ip_addr, "source": "auto-discover"})
                log_application("INFO", f"[INFO] api_refresh_health: auto-discovered mgmt_ip={ip_addr}", device_id=device_id, extra={"component": "api"})
                return jsonify({"ok": True, "status": st["health"]})
            else:
                with device_state_lock:
                    st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
                log_application("WARNING", "[WARNING] api_refresh_health: device row missing during mgmt update", device_id=device_id, extra={"component": "api"})
                return jsonify({"ok": False, "reason": "device ip not found during update"})
        elif int_status == "busy":
            # couldn't find usable ip or interface down
            with device_state_lock:
                st["health"] = "busy"
                st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
            log_application("WARNING", "[WARNING] api_refresh_health: console port busy", device_id=device_id, extra={"component": "api"})
            return jsonify({"ok": True, "status": st["health"], "reason": "Telnet to port failed! Selected hunt group busy.", "raw": res.get("raw")})
        else:
            # couldn't find usable ip or interface down
            with device_state_lock:
                st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
            log_application(
                "WARNING", f"[WARNING] api_refresh_health: no usable mgmt from telnet int_status={int_status!r}",
                device_id=device_id, extra={"component": "api"},
            )
            return jsonify({"ok": False, "reason": "no usable ip or interface not up", "raw": res.get("raw")})

# -----------------------
# Database CRUD Operations
# -------------------------------------------------------------------------------------------------

@app.route("/api/db/add", methods=["POST"])
def api_db_add():
    """Add new device entry to database"""
    body = request.get_json()
    if not body:
        log_application("WARNING", "[WARNING] api_db_add: no JSON body", extra={"component": "api"})
        return jsonify({"ok": False, "error": "No data provided"}), 400
    
    # Validate required fields
    required = ["device_id", "console_ip", "port_id"]
    for field in required:
        if field not in body or not body[field]:
            log_application("WARNING", f"[WARNING] api_db_add: missing field {field}", extra={"component": "api"})
            return jsonify({"ok": False, "error": f"Missing required field: {field}"}), 400
    
    devices = read_devices_from_csv()
    
    # Check for duplicate device_id
    if find_device(devices, body["device_id"]):
        log_application("WARNING", f"[WARNING] api_db_add: duplicate device_id={body['device_id']}", device_id=body["device_id"], extra={"component": "api"})
        return jsonify({"ok": False, "error": "device_id already exists"}), 400
    
    # Validate port_id range
    try:
        port_id = int(body.get("port_id", 0))
        if port_id < 1 or port_id > 64:
            log_application("WARNING", f"[WARNING] api_db_add: invalid port_id={port_id}", extra={"component": "api"})
            return jsonify({"ok": False, "error": "port_id must be between 1-64"}), 400
    except ValueError:
        log_application("WARNING", "[WARNING] api_db_add: port_id not a number", extra={"component": "api"})
        return jsonify({"ok": False, "error": "port_id must be a number"}), 400
    
    # Validate console_ip ipv4
    console_ip = body.get("console_ip", "")
    if not (console_ip and is_valid_ipv4(console_ip)): 
        log_application("WARNING", "[WARNING] api_db_add: invalid console_ip", extra={"component": "api"})
        return jsonify({"ok": False, "error": "missing/invalid console_ip!"}), 400

    # Create new device entry with only updatable fields + defaults
    new_device = {
        "device_id": body.get("device_id", ""),
        "serial_no": body.get("serial_no", ""),
        "model_name": body.get("model_name", ""),
        "hw_id": body.get("hw_id", ""),
        "mgmt_ip": "",  # Will be auto-discovered
        "console_ip": console_ip,
        "port_id": str(port_id),
        "tag": "free",
        "current_user": "",
        "duration": "",
        "resv_end_time": ""
    }
    
    devices.append(new_device)
    write_devices_to_csv(devices)
    
    # Log operation
    log_operation("ADD", new_device["device_id"], {"fields": new_device})
    log_application("INFO", f"[INFO] api_db_add: device added device_id={new_device['device_id']}", device_id=new_device["device_id"], extra={"component": "api"})
    
    # Auto-discover mgmt_ip via telnet
    try:
        if health_manager.ensure_ssh():
            with health_manager.ssh_lock:
                res = telnet_and_run_show_serviceport(health_manager.ssh_client, console_ip,  PORT_OFFSET + port_id)
                if res and res.get("interface_status") == "up" and res.get("ip"):
                    ip_addr = res.get("ip")
                    if is_valid_ipv4(ip_addr):
                        # Update mgmt_ip
                        for d in devices:
                            if d["device_id"] == new_device["device_id"]:
                                d["mgmt_ip"] = ip_addr
                                break
                        write_devices_to_csv(devices)
                        log_operation("EDIT", new_device["device_id"], {"field": "mgmt_ip", "old": "", "new": ip_addr, "source": "auto-discover-on-add"})
                        print(f">>> $ Auto-discovered mgmt_ip {ip_addr} for device {new_device['device_id']}")
                        log_application("INFO", f"[INFO] api_db_add: auto-discovered mgmt_ip={ip_addr}", device_id=new_device["device_id"], extra={"component": "api"})
    except Exception as e:
        print(f">>> $ Auto-discover failed for {new_device['device_id']}: {e}")
        log_application("WARNING", f"[WARNING] api_db_add: auto-discover exception: {e}", device_id=new_device["device_id"], extra={"component": "api"})
    
    return jsonify({"ok": True, "device": new_device})

@app.route("/api/db/edit", methods=["POST"])
def api_db_edit():
    """Edit existing device entry (only UPDATABLE_FIELDS)"""
    body = request.get_json()
    if not body:
        log_application("WARNING", "[WARNING] api_db_edit: no JSON body", extra={"component": "api"})
        return jsonify({"ok": False, "error": "No data provided"}), 400
    
    device_id = body.get("device_id")
    if not device_id:
        log_application("WARNING", "[WARNING] api_db_edit: device_id missing", extra={"component": "api"})
        return jsonify({"ok": False, "error": "device_id required"}), 400
    
    devices = read_devices_from_csv()
    d = find_device(devices, device_id)
    if not d:
        log_application("WARNING", f"[WARNING] api_db_edit: device not found device_id={device_id}", device_id=device_id, extra={"component": "api"})
        return jsonify({"ok": False, "error": "device not found"}), 404
    
    changes = {}
    for field in UPDATABLE_FIELDS:
        if field in body and field != "device_id":  # device_id cannot be changed
            old_val = d.get(field, "")
            new_val = body[field]
            
            # Validate port_id range
            if field == "port_id":
                try:
                    port_id = int(new_val)
                    if port_id < 1 or port_id > 64:
                        log_application("WARNING", f"[WARNING] api_db_edit: invalid port_id={port_id}", device_id=device_id, extra={"component": "api"})
                        return jsonify({"ok": False, "error": "port_id must be between 1-64"}), 400
                except ValueError:
                    log_application("WARNING", "[WARNING] api_db_edit: port_id not a number", device_id=device_id, extra={"component": "api"})
                    return jsonify({"ok": False, "error": "port_id must be a number"}), 400
            
            # Validate console_ip
            console_ip = body.get("console_ip", "")
            if not (console_ip and is_valid_ipv4(console_ip)):
                log_application("WARNING", "[WARNING] api_db_edit: invalid console_ip", device_id=device_id, extra={"component": "api"})
                return jsonify({"ok": False, "error": "missing/invalid console_ip!"}), 400

            if old_val != new_val:
                d[field] = new_val
                changes[field] = {"old": old_val, "new": new_val}
    
    if not changes:
        log_application("INFO", "[INFO] api_db_edit: no field changes", device_id=device_id, extra={"component": "api"})
        return jsonify({"ok": True, "message": "No changes made"})
    
    write_devices_to_csv(devices)
    log_operation("EDIT", device_id, {"fields": changes})
    log_application("INFO", f"[INFO] api_db_edit: saved changes keys={list(changes.keys())}", device_id=device_id, extra={"component": "api"})
    
    # If port_id changed, try to auto-discover mgmt_ip
    if "port_id" in changes or "console_ip" in changes:
        try:
            if health_manager.ensure_ssh():
                with health_manager.ssh_lock:
                    console_ip = d.get("console_ip", 0); port_id = int(d.get("port_id", 0))
                    res = telnet_and_run_show_serviceport(health_manager.ssh_client, console_ip, PORT_OFFSET + port_id)
                    if res and res.get("interface_status") == "up" and res.get("ip"):
                        ip_addr = res.get("ip")
                        if is_valid_ipv4(ip_addr):
                            old_ip = d.get("mgmt_ip", "")
                            d["mgmt_ip"] = ip_addr
                            write_devices_to_csv(devices)
                            log_operation("EDIT", device_id, {"field": "mgmt_ip", "old": old_ip, "new": ip_addr, "source": "auto-discover-on-port-change"})
                            changes["mgmt_ip"] = {"old": old_ip, "new": ip_addr}
        except Exception as e:
            print(f">>> $ Auto-discover failed after port change for {device_id}: {e}")
            log_application("WARNING", f"[WARNING] api_db_edit: auto-discover after port change failed: {e}", device_id=device_id, extra={"component": "api"})
    
    return jsonify({"ok": True, "changes": changes})

@app.route("/api/db/delete", methods=["POST"])
def api_db_delete():
    """Delete device entry from database"""
    body = request.get_json()
    device_id = body.get("device_id")
    if not device_id:
        log_application("WARNING", "[WARNING] api_db_delete: device_id missing", extra={"component": "api"})
        return jsonify({"ok": False, "error": "device_id required"}), 400
    
    devices = read_devices_from_csv()
    d = find_device(devices, device_id)
    if not d:
        log_application("WARNING", f"[WARNING] api_db_delete: device not found device_id={device_id}", device_id=device_id, extra={"component": "api"})
        return jsonify({"ok": False, "error": "device not found"}), 404
    
    # Store device data for logging before deletion
    device_snapshot = dict(d)
    
    devices = [dev for dev in devices if dev["device_id"] != device_id]
    write_devices_to_csv(devices)
    
    # Log operation with full snapshot for potential revert
    log_operation("DELETE", device_id, {"snapshot": device_snapshot})
    
    return jsonify({"ok": True})

@app.route("/api/db/console/ip", methods=["POST"])
def api_db_console_ip():
    """Update console port only (with validation)"""
    body = request.get_json()
    device_id = body.get("device_id")
    console_ip = body.get("console_ip")
    
    if not device_id or console_ip is None:
        return jsonify({"ok": False, "error": "device_id and console_ip required"}), 400
    
    if not (console_ip and is_valid_ipv4(console_ip)):
        return jsonify({"ok": False, "error": "missing/invalid console_ip!"}), 400
    
    devices = read_devices_from_csv()
    d = find_device(devices, device_id)
    if not d:
        return jsonify({"ok": False, "error": "device not found"}), 404
    
    old_ip= d.get("console_ip", "")
    d["console_ip"] = console_ip
    write_devices_to_csv(devices)
    
    log_operation("EDIT", device_id, {"field": "port_id", "old": old_ip, "new": console_ip})
    log_application("INFO", f"[INFO] api_db_console/ip: console_ip set to {console_ip!r}", device_id=device_id, extra={"component": "api"})
    
    # Auto-discover mgmt_ip with new port
    mgmt_discovered = None
    port_id = int(d.get("port_id", ""))
    try:
        if health_manager.ensure_ssh():
            with health_manager.ssh_lock:
                res = telnet_and_run_show_serviceport(health_manager.ssh_client, console_ip, PORT_OFFSET + port_id)
                if res and res.get("interface_status") == "up" and res.get("ip"):
                    ip_addr = res.get("ip")
                    if is_valid_ipv4(ip_addr):
                        old_ip = d.get("mgmt_ip", "")
                        d["mgmt_ip"] = ip_addr
                        write_devices_to_csv(devices)
                        log_operation("EDIT", device_id, {"field": "mgmt_ip", "old": old_ip, "new": ip_addr, "source": "auto-discover-on-port-change"})
                        mgmt_discovered = ip_addr
    except Exception as e:
        print(f">>> $ Auto-discover failed for {device_id}: {e}")
        log_application("WARNING", f"[WARNING] api_db_console/ip: auto-discover failed: {e}", device_id=device_id, extra={"component": "api"})
    
    return jsonify({"ok": True, "mgmt_ip": mgmt_discovered})

@app.route("/api/db/console/port", methods=["POST"])
def api_db_console_port():
    """Update console port only (with validation)"""
    body = request.get_json()
    device_id = body.get("device_id")
    
    if not device_id:
        log_application("WARNING", "[WARNING] api_db_console/port: device_id missing", extra={"component": "api"})
        return jsonify({"ok": False, "error": "device_id required"}), 400
    
    try:
        port_id = int(body.get("port_id"))
        if port_id < 1 or port_id > 64:
            log_application("WARNING", f"[WARNING] api_db_console/port: invalid port_id={port_id}", device_id=device_id, extra={"component": "api"})
            return jsonify({"ok": False, "error": "port_id must be between 1-64"}), 400
    except ValueError:
        log_application("WARNING", "[WARNING] api_db_console/port: port_id not a number", device_id=device_id, extra={"component": "api"})
        return jsonify({"ok": False, "error": "port_id must be a number"}), 400
    
    devices = read_devices_from_csv()
    d = find_device(devices, device_id)
    if not d:
        log_application("WARNING", f"[WARNING] api_db_console/port: device not found device_id={device_id}", device_id=device_id, extra={"component": "api"})
        return jsonify({"ok": False, "error": "device not found"}), 404
    
    old_port = d.get("port_id", "")
    d["port_id"] = str(port_id)
    write_devices_to_csv(devices)
    
    log_operation("EDIT", device_id, {"field": "port_id", "old": old_port, "new": str(port_id)})
    log_application("INFO", f"[INFO] api_db_console/port: port_id set to {port_id}", device_id=device_id, extra={"component": "api"})
    
    # Auto-discover mgmt_ip with new port
    mgmt_discovered = None
    console_ip = d.get("console_ip", "")
    try:
        if health_manager.ensure_ssh():
            with health_manager.ssh_lock:
                res = telnet_and_run_show_serviceport(health_manager.ssh_client, console_ip, PORT_OFFSET + port_id)
                if res and res.get("interface_status") == "up" and res.get("ip"):
                    ip_addr = res.get("ip")
                    if is_valid_ipv4(ip_addr):
                        old_ip = d.get("mgmt_ip", "")
                        d["mgmt_ip"] = ip_addr
                        write_devices_to_csv(devices)
                        log_operation("EDIT", device_id, {"field": "mgmt_ip", "old": old_ip, "new": ip_addr, "source": "auto-discover-on-port-change"})
                        mgmt_discovered = ip_addr
    except Exception as e:
        print(f">>> $ Auto-discover failed for {device_id}: {e}")
        log_application("WARNING", f"[WARNING] api_db_console/port: auto-discover failed: {e}", device_id=device_id, extra={"component": "api"})
    
    return jsonify({"ok": True, "mgmt_ip": mgmt_discovered})

# -------------------------------------------------------------------------------------------------

def start_background_services():
    init_device_state_from_csv()
    health_manager.start()
    reservation_monitor.start()
    log_application("INFO", "[INFO] background service threads started", extra={"component": "startup"})

def kill_process_on_port(port):
    """Forcefully kills any process running on the specified port (Linux/Unix)."""
    try:
        # Find PIDs using the port
        result = subprocess.check_output(["lsof", "-t", f"-i:{port}"])
        pids = result.decode().strip().split('\n')
        for pid in pids:
            if pid:
                print(f">>> $ Forcefully clearing port {port} (Killing PID {pid})...")
                subprocess.run(["kill", "-9", pid])
    except subprocess.CalledProcessError:
        # No process was using the port
        pass
    except Exception as e:
        print(f">>> $ Note: Could not auto-kill process on port {port}: {e}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Switch Reservation UI")
    parser.add_argument("--debug", action="store_true", default=False, help="Enable Flask debug mode")
    parser.add_argument('-p', "--port", type=int, default=7000, help="Port number to run the server on")
    args = parser.parse_args()

    # 1. Kill any existing process on port 5000
    kill_process_on_port(args.port)
    
    # 2. Start background services
    start_background_services()
    
    # 3. Run Flask (use_reloader=False is safer for manual port management)
    log_application(
        "INFO", f"[INFO] Flask listen host=0.0.0.0 port={args.port} debug={args.debug}",
        extra={"component": "startup", "port": args.port, "debug": args.debug},
    )
    app.run(host="0.0.0.0", port=args.port, debug=args.debug, use_reloader=False, threaded=True)
