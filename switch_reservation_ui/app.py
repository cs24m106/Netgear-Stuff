# app.py
import time, datetime
import os, threading, csv
import ipaddress, socket, subprocess
from filelock import FileLock
from flask import Flask, jsonify, request, render_template, send_from_directory
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

OfficeIP = NUKE_IP # set whichever is currently working here
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

# local database
CSV_PATH = "database.csv"
CSV_LOCK_PATH = CSV_PATH + ".lock"
UPDATABLE_FIELDS = ["device_id", "serial_no", "model_name", "hw_id", "port_id"]
# audit trail 
OPERATION_LOG_PATH = "operations.log"
OPERATION_LOG_LOCK = OPERATION_LOG_PATH + ".lock"

# -----------------------
# Backend state
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
                if not r.get("tag"):
                    r["tag"] = "free"
                devices.append(r)
    return devices

def write_devices_to_csv(devices):
    lock = FileLock(CSV_LOCK_PATH, timeout=10)
    with lock:
        with open(CSV_PATH, "w", newline='') as f:
            fieldnames = ["device_id", "serial_no", "model_name", "hw_id", "mgmt_ip", "port_id", "tag", "current_user", "duration", "resv_end_time"]
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for d in devices:
                if not d.get("tag"):
                    d["tag"] = "free"
                # Ensure all fields exist
                for field in fieldnames:
                    if field not in d:
                        d[field] = ""
                writer.writerow(d)

def find_device(devices, device_id):
    for d in devices:
        if d["device_id"] == device_id:
            return d
    return None

def log_operation(operation_type, device_id, before_state=None, after_state=None, user="system"):
    """
    Log database operations in a reversible format.
    Format: TIMESTAMP|OPERATION|DEVICE_ID|BEFORE|AFTER|USER
    """
    import json
    from datetime import datetime
    
    timestamp = datetime.utcnow().isoformat()
    log_entry = {
        "timestamp": timestamp,
        "operation": operation_type,  # ADD, EDIT, DELETE
        "device_id": device_id,
        "before": before_state,
        "after": after_state,
        "user": user
    }
    
    lock = FileLock(OPERATION_LOG_LOCK, timeout=10)
    with lock:
        with open(OPERATION_LOG_PATH, "a", newline='') as f:
            f.write(json.dumps(log_entry) + "\n")
    
    if app.debug:
        print(f"[DEBUG] Operation logged: {operation_type} for {device_id}")


# -----------------------
# Telnet/SSH helpers (interact with console via singleton SSH client shell)
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

def telnet_and_run_show_serviceport(ssh_client, device_port, login_user=SWITCH_USER, login_pass=SWITCH_PASSWORD):
    """
    Telnet to CONSOLE_IP:device_port using an interactive shell (invoke_shell).
    Handles optional User:/Password: prompts, attempts to reach switch CLI prompt '>' or '#'.
    If it reaches prompt, runs 'show serviceport' and returns parsed result dict:
      {"interface_status": "Up"/"Down"/None, "ip": "x.x.x.x"/None, "raw": "<output>"}
    On failure returns None.
    """
    try:
        if app.debug:
            print(f"[DEBUG] telnet_and_run_show_serviceport: console_ip={CONSOLE_IP}, device_port={device_port}")

        if not device_port:
            if app.debug:
                print("[DEBUG] device_port is empty, aborting telnet attempt.")
            return False
        chan = ssh_client.invoke_shell()
        time.sleep(0.1)
        if app.debug:
            print("[DEBUG] Starting telnet session...")
        # Start telnet
        chan.send(f"telnet {CONSOLE_IP} {device_port}\n")
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

        # Now look for a CLI prompt ')>' or ')#'
        # Give it a few seconds to settle
        out += _read_channel_until(chan, timeout_sec=.5)
        if app.debug:
            print(f"[DEBUG] After waiting for CLI prompt:\n{out!r}")

        int_status = None
        # If we did not get a prompt yet, try sending a newline to elicit it
        if not re.search(r"\)[>#]\s*$", out):
            int_status = "busy"
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
        if re.search(r"\)[>#]\s*$", out):
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
def ssh_and_set_hostname_over_pi(ssh_client, mgmt_ip, new_hostname, serial_no='', login_user=SWITCH_USER, login_pass=SWITCH_PASSWORD):
    """
    Use the existing ssh_client (connected to the PI/console) and from that shell run:
        ssh {login_user}@{mgmt_ip}
    Then perform:
        en
        hostname {new_hostname}
        logout

    Returns True if the hostname change is detected (prompt contains new_hostname# or (new_hostname)#), False otherwise.
    """
    try:
        if app.debug:
            print(f"[DEBUG] ssh_and_set_hostname_over_pi: mgmt_ip={mgmt_ip}, new_hostname={new_hostname}")

        if not mgmt_ip:
            if app.debug:
                print("[DEBUG] mgmt_ip is empty, aborting ssh attempt.")
            return False, "mgmt_ip not found!"

        chan = ssh_client.invoke_shell()
        time.sleep(0.1)

        # Start nested ssh
        chan.send(f"ssh {login_user}@{mgmt_ip}\n")
        out = _read_channel_until(chan, timeout_sec=1)

        # Handle first-time hostkey prompt (supports both old and new OpenSSH formats)
        hostkey_patterns = [
            r"are you sure you want to continue connecting \(yes/no(?:/\[fingerprint\])?\)\s*$",
            r"ecdsa key fingerprint is sha256:[a-zA-Z0-9+/=]+",
            r"this key is not known by any other names",
        ]
        hostkey_detected = any(re.search(pat, out, re.IGNORECASE | re.MULTILINE) for pat in hostkey_patterns)
        
        if hostkey_detected:
            if app.debug:
                print("[DEBUG] Hostkey prompt detected (new or old format), sending 'yes'")
            chan.send("yes\n")
            out += _read_channel_until(chan, timeout_sec=1)  # Increased timeout for key exchange

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
            
            chan.send("show version\n")
            out += _read_channel_until(chan, timeout_sec=2)
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
            print(f"[DEBUG] Exception in ssh_and_set_hostname_over_pi: {e}")
            traceback.print_exc()
        chan.close()
        return False, f"Exception: {e}"

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
                    "health": "unk",
                    "retry_count": 0,
                    "next_check_ts": time.time() + 1
                }

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
                return True
            except Exception:
                self.ssh_client = None
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
            for device_id, mgmt_ip in check_list:
                with device_state_lock:
                    st = device_state.get(device_id)
                    if not st:
                        continue
                    st["retry_count"] += 1
                    st["health"] = "down"
                    st["next_check_ts"] = time.time() + DOWN_HEALTH_TIMER
            return

        with self.ssh_lock:
            client = self.ssh_client
            for device_id, mgmt_ip in check_list:
                try:
                    ok = ssh_ping_once(client, mgmt_ip)
                except Exception:
                    ok = False
                with device_state_lock:
                    st = device_state.get(device_id)
                    if not st:
                        continue
                    if ok:
                        st["health"] = "up"
                        st["retry_count"] = 0
                        st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
                    else:
                        st["retry_count"] += 1
                        st["health"] = "down"
                        if st["retry_count"] < MAX_HEALTH_CHECK_RETRIES:
                            st["next_check_ts"] = time.time() + DOWN_HEALTH_TIMER
                        else:
                            st["next_check_ts"] = time.time() + UP_HEALTH_TIMER

    def run(self):
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
                    self.run_one_cycle(to_ping)
                else:
                    time.sleep(0.5) # sleep half sec when check list is empty
            except Exception:
                traceback.print_exc()
                self.close_ssh()
                time.sleep(self.reconnect_backoff)

    def stop(self):
        self.keep_running = False
        self.close_ssh()

health_manager = HealthManager(OfficeIP, SSH_USER, SSH_PASSWORD) # init manager

# -----------------------
# Reservation Monitor (auto-release)
# -------------------------------------------------------------------------------------------------
class ReservationMonitor(threading.Thread):
    def __init__(self):
        super().__init__(daemon=True)
        self.keep_running = True

    def run(self):
        while self.keep_running:
            try:
                # 1. Read Database
                devices = read_devices_from_csv()
                dirty = False
                now = datetime.datetime.utcnow()

                # 2. Check for expirations
                for d in devices:
                    if d.get("tag") == "resv":
                        end_str = d.get("resv_end_time")
                        if end_str:
                            try:
                                end_dt = datetime.datetime.fromisoformat(end_str)
                                # Buffer of 1 second to ensure we don't race with the very last second of display
                                if now >= end_dt:
                                    print(f" * [Auto-Release] Expired reservation for {d['device_id']}. Releasing...")

                                    # Capture model_name & port_id before clearing fields
                                    model_name = (d.get("model_name") or "").strip()
                                    mgmt = d.get("mgmt_ip") or ""
                                    try:
                                        port_id = int(d.get("port_id", 0))
                                    except Exception:
                                        port_id = 0

                                    # Mark as released locally (CSV will be written later)
                                    d["tag"] = "free"
                                    d["current_user"] = ""
                                    d["duration"] = ""
                                    d["resv_end_time"] = ""
                                    dirty = True

                                    # Best-effort: restore hostname on physical switch back to model_name
                                    # Use the singleton SSH client guarded by health_manager.ssh_lock
                                    if port_id and model_name and mgmt:
                                        serial_no = d.get("serial_no") or ""
                                        if health_manager.ensure_ssh():
                                            with health_manager.ssh_lock:
                                                ok,_ = ssh_and_set_hostname_over_pi(health_manager.ssh_client, mgmt, model_name, serial_no)
                                                if ok:
                                                    print(f" * Hostname restored to {model_name} for device {d['device_id']} (auto-release).")
                                                else:
                                                    print(f" * Hostname restore to {model_name} failed for device {d['device_id']} (auto-release).")
                                                
                                        else:
                                            print(f" * Could not open SSH to PI to restore hostname for device {d['device_id']}.")

                            except ValueError:
                                # Handle cases where date format might be corrupted
                                pass
                
                # 3. Save if changes made
                if dirty:
                    write_devices_to_csv(devices)
                
            except Exception:
                traceback.print_exc()
            # Check every 5 seconds to reduce I/O load (Backoff on error)
            time.sleep(5) 

reservation_monitor = ReservationMonitor() # init monitor

# -----------------------
# Flask app + API
# -------------------------------------------------------------------------------------------------
app = Flask(__name__, static_folder="static", template_folder="templates")

@app.route("/")
def index():
    #return render_template("index.html") # version-1
    return render_template("minimal.html") # version-2

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

@app.route("/api/devices/view", methods=["GET"])
def api_devices_view():
    devices = read_devices_from_csv()
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

@app.route("/api/remove_mgmt_ip", methods=["POST"])
def api_remove_mgmt_ip():
    body = request.get_json()
    device_id = body.get("device_id")
    if not device_id:
        return jsonify({"ok": False, "error": "device_id required"}), 400
    devices = read_devices_from_csv()
    d = find_device(devices, device_id)
    if not d:
        return jsonify({"ok": False, "error": "device not found"}), 404
    d["mgmt_ip"] = ""
    write_devices_to_csv(devices)
    return jsonify({"ok": True})

@app.route("/api/refresh_health", methods=["POST"])
def api_refresh_health():
    body = request.get_json()
    device_id = body.get("device_id")
    if not device_id:
        return jsonify({"error": "device_id required"}), 400

    # load CSV to find related metadata (port_id, mgmt_ip)
    devices = read_devices_from_csv()
    dev = find_device(devices, device_id)
    port_id = None
    mgmt_ip = None
    if dev:
        try:
            port_id = int(dev.get("port_id", 0))
        except Exception:
            port_id = None
        mgmt_ip = dev.get("mgmt_ip") or ""

    with device_state_lock:
        st = device_state.get(device_id)
        if not st:
            # initialize unknown state and schedule immediate handling
            device_state[device_id] = {
                "health": "unk",
                "retry_count": 0,
                "next_check_ts": time.time() + 0.1
            }
            return jsonify({"ok": True})

        health = st.get("health", "unk").lower()
    
    # Branch by health -> if health == "up" or health == "down", its mgmt_ip should be present
    if mgmt_ip and is_valid_ipv4(mgmt_ip):
        # Try one immediate ping if we can (best-effort)
        if health_manager.ensure_ssh():
            with health_manager.ssh_lock:
                try:
                    ok = ssh_ping_once(health_manager.ssh_client, mgmt_ip, count=2, timeout=4)
                except Exception:
                    ok = False
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
            
            return jsonify({"ok": True, "status": st["health"]})
        else:
            return jsonify({"ok": False,  "reason": "unable to open ssh to pi"})
    
    # if health == "unk" --> most likely mgmt_ip not present
    else: 
        # Attempt telnet to console to discover mgmt IP and update if Interface Status is Up
        if port_id is None:
            # can't telnet without port info
            # initialize state to re-check later
            with device_state_lock:
                st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
            return jsonify({"ok": False, "reason": "missing port_id, need manual input"})

        # Use health_manager's ssh client (singleton). Best-effort.
        if not health_manager.ensure_ssh():
            # schedule retry
            with device_state_lock:
                st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
            return jsonify({"ok": False, "reason": "unable to open ssh to pi"})

        with health_manager.ssh_lock:
            try:
                res = telnet_and_run_show_serviceport(health_manager.ssh_client, PORT_OFFSET + port_id)
            except Exception:
                res = None

        if not res:
            # failed to get info — schedule re-check later
            with device_state_lock:
                st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
            return jsonify({"ok": False, "reason": "telnet did not return data"})

        # If Interface Status is up and IP found, update CSV mgmt_ip and schedule immediate ping
        int_status = (res.get("interface_status") or "").lower() if res.get("interface_status") else None
        ip_addr = res.get("ip")
        if int_status == "up" and ip_addr and is_valid_ipv4(ip_addr):
            # Update CSV (atomic via FileLock) and device_state to ping immediately
            devices = read_devices_from_csv()
            d = find_device(devices, device_id)
            if d:
                d["mgmt_ip"] = ip_addr
                write_devices_to_csv(devices)
                with device_state_lock:
                    st["health"] = "unk"   # keep as unk until ping completes
                    st["retry_count"] = 0
                    st["next_check_ts"] = time.time() + 0.1
                return jsonify({"ok": True, "status": st["health"]})
            else:
                with device_state_lock:
                    st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
                return jsonify({"ok": False, "reason": "device not found during update"})
        elif int_status == "busy":
            # couldn't find usable ip or interface down
            with device_state_lock:
                st["health"] = "busy"
                st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
            return jsonify({"ok": True, "status": st["health"], "reason": "Telnet to port failed! Selected hunt group busy.", "raw": res.get("raw")})
        else:
            # couldn't find usable ip or interface down
            with device_state_lock:
                st["next_check_ts"] = time.time() + UP_HEALTH_TIMER
            return jsonify({"ok": False, "reason": "no usable ip or interface not up", "raw": res.get("raw")})

# -------------------------------------------------------------------------------------------------

@app.route("/api/reserve", methods=["POST"])
def api_reserve():
    body = request.get_json()
    device_id = body.get("device_id")
    user = body.get("user")
    hours = int(body.get("hours", 0))
    minutes = int(body.get("minutes", 0))
    duration_minutes = hours * 60 + minutes
    if not device_id or not user:
        return jsonify({"error": "device_id and user required"}), 400
    devices = read_devices_from_csv()
    d = find_device(devices, device_id)
    if not d:
        return jsonify({"error": "device not found"}), 404
    
    before_state = dict(d) # Capture state before reservation changes
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
    port_id = int(d.get("port_id", 0))
    if not port_id:
        return jsonify({"error": "port_id not found"}), 404
    
    ok = False
    msg = ''
    # try to set hostname to current user (trim all whitespace)
    new_name = device_id + '-' + re.sub(r"\s+", "", d.get("current_user") or "")
    mgmt = d.get("mgmt_ip") or ""
    serial_no = d.get("serial_no") or ""

    if new_name and mgmt:
        if health_manager.ensure_ssh():
            with health_manager.ssh_lock:
                try:
                    ok,msg = ssh_and_set_hostname_over_pi(health_manager.ssh_client, mgmt, new_name, serial_no)
                except Exception:
                    traceback.print_exc()
    if ok:
        print(f">>> $ Hostname set to {new_name} for device {device_id}")
        write_devices_to_csv(devices) # save if reservation is successful
        log_operation("RESERVE", device_id, before_state=before_state, after_state=dict(d), user=d.get("current_user", "system"))
    else:
        print(f">>> $ Hostname change to {new_name} failed for device {device_id}")
        
    return jsonify({"ok": ok, "resv_end_time": d["resv_end_time"], "msg": msg})

@app.route("/api/release", methods=["POST"])
def api_release():
    body = request.get_json()
    device_id = body.get("device_id")
    if not device_id:
        return jsonify({"error": "device_id required"}), 400
    devices = read_devices_from_csv()
    d = find_device(devices, device_id)
    if not d:
        return jsonify({"error": "device not found"}), 404
    # Save model_name before overwriting fields (we need it to restore hostname)
    model_name = (d.get("model_name") or "").strip()
    mgmt = d.get("mgmt_ip") or ""
    
    before_state = dict(d) # Capture state before reservation changes
    d["current_user"] = ""
    d["duration"] = ""
    d["resv_end_time"] = ""
    d["tag"] = "free"
    write_devices_to_csv(devices) # for release updating on device host is optional
    log_operation("RELEASE", device_id, before_state=before_state, after_state=dict(d)) # Log release operation

    # Attempt to change hostname back to model_name via telnet
    try:
        port_id = int(d.get("port_id", 0))
    except Exception:
        port_id = 0
    ok = False
    if port_id and model_name and mgmt:
        serial_no = d.get("serial_no") or ""
        if health_manager.ensure_ssh():
            with health_manager.ssh_lock:
                try:
                    ok = ssh_and_set_hostname_over_pi(health_manager.ssh_client, mgmt, model_name, serial_no)
                    if ok:
                        print(f" * Hostname restored to {model_name} for device {device_id}")
                    else:
                        print(f" * Hostname restore to {model_name} failed for device {device_id}")
                except Exception:
                    traceback.print_exc()

    return jsonify({"ok": ok})

@app.route("/api/devices/add", methods=["POST"])
def api_devices_add():
    """Add a new device to the database."""
    body = request.get_json()
    
    # Required fields
    for field in UPDATABLE_FIELDS:
        if not body.get(field):
            return jsonify({"ok": False, "error": f"Missing required field: {field}"}), 400
    
    devices = read_devices_from_csv()
    
    # Check for duplicate device_id
    if find_device(devices, body["device_id"]):
        return jsonify({"ok": False, "error": "Device ID already exists"}), 409
    
    # Create new device record
    new_device = {
        "device_id": body["device_id"],
        "serial_no": body.get("serial_no", ""),
        "model_name": body["model_name"],
        "hw_id": body.get("hw_id", ""),
        "mgmt_ip": body["mgmt_ip"],
        "port_id": str(body["port_id"]),
        "tag": "free",
        "current_user": "",
        "duration": "",
        "resv_end_time": ""
    }
    
    # Log operation (before state is None for ADD)
    log_operation("ADD", new_device["device_id"], before_state=None, after_state=new_device)
    
    # Add to list and write
    devices.append(new_device)
    write_devices_to_csv(devices)
    
    # Update device state
    with device_state_lock:
        device_state[new_device["device_id"]] = {
            "health": "unk",
            "retry_count": 0,
            "next_check_ts": time.time() + 1
        }
    
    return jsonify({"ok": True, "device_id": new_device["device_id"]})

@app.route("/api/devices/edit", methods=["POST"])
def api_devices_edit():
    """Edit an existing device."""
    body = request.get_json()
    device_id = body.get("device_id")
    
    if not device_id:
        return jsonify({"ok": False, "error": "device_id required"}), 400
    
    devices = read_devices_from_csv()
    d = find_device(devices, device_id)
    
    if not d:
        return jsonify({"ok": False, "error": "Device not found"}), 404
    
    # Capture before state for logging
    before_state = dict(d)
    
    # Update fields (only allow certain fields to be edited)
    for field in UPDATABLE_FIELDS:
        if field in body:
            d[field] = body[field]
    
    # Log operation
    log_operation("EDIT", device_id, before_state=before_state, after_state=dict(d))
    
    write_devices_to_csv(devices)
    
    return jsonify({"ok": True, "device_id": device_id})

@app.route("/api/devices/delete", methods=["POST"])
def api_devices_delete():
    """Delete a device from the database."""
    body = request.get_json()
    device_id = body.get("device_id")
    
    if not device_id:
        return jsonify({"ok": False, "error": "device_id required"}), 400
    
    devices = read_devices_from_csv()
    d = find_device(devices, device_id)
    
    if not d:
        return jsonify({"ok": False, "error": "Device not found"}), 404
    
    # Capture before state for logging (allows reconstruction)
    before_state = dict(d)
    
    # Log operation
    log_operation("DELETE", device_id, before_state=before_state, after_state=None)
    
    # Remove from list
    devices = [dev for dev in devices if dev["device_id"] != device_id]
    write_devices_to_csv(devices)
    
    # Clean up device state
    with device_state_lock:
        if device_id in device_state:
            del device_state[device_id]
    
    return jsonify({"ok": True, "device_id": device_id})

# -------------------------------------------------------------------------------------------------

@app.route("/api/config", methods=["POST"])
def api_config():
    data = request.get_json()
    mgmt_ip = data.get("mgmt_ip")
    if not mgmt_ip:
        return jsonify({"error":"mgmt_ip required"}), 400
    
    port_id = 0
    try:
        port_id = int(data.get("port_id"))
    except Exception as e:
        return jsonify({"error":e}), 400

    # Extract last octet (e.g., 192.168.1.55 -> 55)
    last_octet = int(mgmt_ip.strip().split(".")[-1])
    
    # Calculate ports based on global config offsets
    av_port = AV_UI["offset"] + last_octet
    old_main_port = MAIN_UI_OLD["offset"] + last_octet
    new_main_port = MAIN_UI["offset"] + last_octet

    return jsonify({
        "switch_id": last_octet,
        "av_port": av_port,
        "new_main_port": new_main_port,
        "old_main_port": old_main_port,
        "console_ip": CONSOLE_IP,
        "device_port": PORT_OFFSET + port_id,
    })

@app.route('/static/<path:p>')
def static_files(p):
    return send_from_directory('static', p)

# -------------------------------------------------------------------------------------------------

def start_background_services():
    init_device_state_from_csv()
    health_manager.start()
    reservation_monitor.start()

def kill_process_on_port(port):
    """Forcefully kills any process running on the specified port (Linux/Unix)."""
    try:
        # Find PIDs using the port
        result = subprocess.check_output(["lsof", "-t", f"-i:{port}"])
        pids = result.decode().strip().split('\n')
        for pid in pids:
            if pid:
                print(f" * Forcefully clearing port {port} (Killing PID {pid})...")
                subprocess.run(["kill", "-9", pid])
    except subprocess.CalledProcessError:
        # No process was using the port
        pass
    except Exception as e:
        print(f" * Note: Could not auto-kill process on port {port}: {e}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Switch Reservation UI")
    parser.add_argument("--debug", action="store_true", default=False, help="Enable Flask debug mode")
    args = parser.parse_args()

    # 1. Kill any existing process on port 5000
    kill_process_on_port(5000)
    
    # 2. Start background services
    start_background_services()
    
    # 3. Run Flask (use_reloader=False is safer for manual port management)
    app.run(host="0.0.0.0", port=5000, debug=args.debug, use_reloader=False)