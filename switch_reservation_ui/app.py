# app.py
import time, datetime
import os, threading
import csv
import ipaddress
import socket, subprocess
from filelock import FileLock
from flask import Flask, jsonify, request, render_template, send_from_directory
import paramiko
import traceback

# -----------------------
# Global Config (change accordingly)
# -----------------------
PI_IP = "10.25.4.200"
SSH_USER = "host1"
SSH_PASSWORD = "sheldon123"
CONSOLE_IP = "192.168.1.102"
PORT_OFFSET = 10000

AV_UI = {"port": 4443, "offset": 60000}
MAIN_UI = {"port": 49152, "offset": 51000}
MAIN_UI_OLD = {"port": 49151, "offset": 50000}

UP_HEALTH_TIMER = 10        # seconds
DOWN_HEALTH_TIMER = 2       # seconds
MAX_HEALTH_CHECK_RETRIES = 3

CSV_PATH = "database.csv"
CSV_LOCK_PATH = CSV_PATH + ".lock"

# -----------------------
# Backend state
# -----------------------
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
            fieldnames = ["device_id","model_name","hw_id","mgmt_ip","port_id","tag","current_user","duration","resv_end_time"]
            writer = csv.DictWriter(f, fieldnames=fieldnames)
            writer.writeheader()
            for d in devices:
                if not d.get("tag"):
                    d["tag"] = "free"
                writer.writerow(d)

def find_device(devices, device_id):
    for d in devices:
        if d["device_id"] == device_id:
            return d
    return None

# -----------------------
# Health manager (single SSH session -> sequential pings)
# -----------------------
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

def ssh_ping_once(ssh_client, mgmt_ip, count=2, timeout=6):
    try:
        cmd = f"ping -c {count} -w {timeout} {mgmt_ip}"
        stdin, stdout, stderr = ssh_client.exec_command(cmd, timeout=timeout + 5)
        exit_status = stdout.channel.recv_exit_status()
        return exit_status == 0
    except Exception:
        return False

class HealthManager(threading.Thread):
    def __init__(self, pi_ip, ssh_user, ssh_password):
        super().__init__(daemon=True)
        self.pi_ip = pi_ip
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
                client.connect(self.pi_ip, username=self.ssh_user, password=self.ssh_password, timeout=10)
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
                    time.sleep(0.7)
            except Exception:
                traceback.print_exc()
                self.close_ssh()
                time.sleep(self.reconnect_backoff)

    def stop(self):
        self.keep_running = False
        self.close_ssh()

health_manager = HealthManager(PI_IP, SSH_USER, SSH_PASSWORD)

# -----------------------
# To handle auto-release, we need a background thread that periodically 
# checks the database for expired timestamps and releases them.
# -----------------------
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
                                    d["tag"] = "free"
                                    d["current_user"] = ""
                                    d["duration"] = ""
                                    d["resv_end_time"] = ""
                                    dirty = True
                            except ValueError:
                                # Handle cases where date format might be corrupted
                                pass
                
                # 3. Save if changes made
                if dirty:
                    write_devices_to_csv(devices)
                
                # Check every 10 seconds to reduce I/O load
                time.sleep(10)
            except Exception:
                traceback.print_exc()
                time.sleep(10) # Backoff on error

# Initialize the monitor
reservation_monitor = ReservationMonitor()

# -----------------------
# Flask app + API
# -----------------------
app = Flask(__name__, static_folder="static", template_folder="templates")

@app.route("/")
def index():
    return render_template("index.html")

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

    write_devices_to_csv(devices)
    return jsonify({"ok": True, "resv_end_time": d["resv_end_time"]})


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
    d["current_user"] = ""
    d["duration"] = ""
    d["resv_end_time"] = ""
    d["tag"] = "free"
    write_devices_to_csv(devices)
    return jsonify({"ok": True})


@app.route("/api/refresh_health", methods=["POST"])
def api_refresh_health():
    body = request.get_json()
    device_id = body.get("device_id")
    if not device_id:
        return jsonify({"error": "device_id required"}), 400
    with device_state_lock:
        st = device_state.get(device_id)
        if not st:
            device_state[device_id] = {
                "health": "unk",
                "retry_count": 0,
                "next_check_ts": time.time() + 0.1
            }
        else:
            st["retry_count"] = 0
            st["next_check_ts"] = time.time() + 0.1
    return jsonify({"ok": True})


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
    # 1. Kill any existing process on port 5000
    kill_process_on_port(5000)
    
    # 2. Start background services
    start_background_services()
    
    # 3. Run Flask (use_reloader=False is safer for manual port management)
    app.run(host="0.0.0.0", port=5000, debug=True, use_reloader=False)