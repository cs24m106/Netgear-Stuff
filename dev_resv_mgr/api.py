"""API routes (devices, DB, HTTP logging) and operations-log statistics."""
import csv
import datetime
import io
import json
import re
import time
import traceback
from pathlib import Path

from filelock import FileLock
from flask import (
    Blueprint,
    Response,
    g,
    jsonify,
    render_template,
    request,
    send_from_directory,
)

from config import *
from utils import (
    add_device,
    delete_device,
    health_manager,
    log_application,
    log_operation,
    read_devices,
    reservation_actor_user,
    ssh_and_set_hostname,
    ssh_ping_once,
    telnet_and_run_show_serviceport,
    update_dynm,
    update_resv,
)

bp = Blueprint("main", __name__)


# --- stats_service (parse / filter / aggregate operations.log) ---
from collections import Counter

def _parse_log_row(line):
    line = line.strip()
    if not line:
        return None, "empty"
    try:
        return json.loads(line), None
    except Exception:
        return None, "parse"

def _row_ts(row):
    if not row:
        return None
    try:
        return datetime.datetime.fromisoformat(row.get("timestamp", "").replace("Z", "+00:00"))
    except Exception:
        return None

def _time_floor(preset):
    now = datetime.datetime.utcnow()
    if preset == "week":
        return now - datetime.timedelta(days=7)
    if preset == "month":
        return now - datetime.timedelta(days=30)
    return None

def _physical_segment(raw_lines, segment, max_lines):
    """Return raw_lines[start:end] for segment 0 = newest tail max_lines."""
    L = len(raw_lines)
    if L == 0:
        return [], 0, L, False, False
    max_lines = max(1, int(max_lines))
    seg = max(0, int(segment))
    end = L - seg * max_lines
    start = max(0, end - max_lines)
    if end <= 0:
        return [], start, end, False, False
    has_next = seg > 0  # toward newer
    has_prev = start > 0  # toward older
    return raw_lines[start:end], start, end, has_prev, has_next

def _row_matches_filters(row, q):
    if not row:
        return False
    ts = _row_ts(row)
    if q.get("time_floor") and ts and ts < q["time_floor"]:
        return False
    if q.get("device_id") and q["device_id"] not in str(row.get("device_id") or ""):
        return False
    if q.get("user") and q["user"] not in str(row.get("user") or ""):
        return False
    if q.get("operation") and row.get("operation") != q["operation"]:
        return False
    ch = row.get("changes") or {}
    if q.get("component") and (ch.get("component") or "") != q["component"]:
        return False
    if q.get("subsystem") and (ch.get("subsystem") or "") != q["subsystem"]:
        return False
    if q.get("level") and (ch.get("level") or "") != q["level"]:
        return False
    if q.get("path_contains") and q["path_contains"] not in str(ch.get("path") or ""):
        return False
    return True

def _aggregate(rows):
    by_op = Counter()
    by_level = Counter()
    by_comp = Counter()
    by_user = Counter()
    by_device = Counter()
    timeline = Counter()
    http_status = Counter()
    for row in rows:
        by_op[row.get("operation") or "unknown"] += 1
        ch = row.get("changes") or {}
        by_level[ch.get("level") or "none"] += 1
        by_comp[ch.get("component") or "unknown"] += 1
        by_user[row.get("user") or "unknown"] += 1
        did = row.get("device_id") or "-"
        if did != "-":
            by_device[did] += 1
        ts = _row_ts(row)
        if ts:
            timeline[ts.strftime("%Y-%m-%d")] += 1
        if ch.get("phase") == "response" and ch.get("status_code") is not None:
            http_status[str(ch["status_code"])] += 1
    def top_n(cnt, n=15):
        return [{"key": k, "count": v} for k, v in cnt.most_common(n)]
    days = sorted(timeline.keys())
    return {
        "by_operation": dict(by_op),
        "by_level": dict(by_level),
        "by_component": dict(by_comp),
        "by_user": top_n(by_user, 20),
        "by_device": top_n(by_device, 20),
        "timeline": [{"day": d, "count": timeline[d]} for d in days],
        "http_status": dict(http_status),
    }

def read_and_filter_opslog(args):
    """
    Read JSONL under FileLock. For heavy multi-reader + huge logs, consider dual-writing
    the same events to SQLite (indexed) and querying that for statistics instead.
    """
    lock = FileLock(LOG_LOCK, timeout=10)
    parse_errors = 0
    raw_lines = []
    with lock:
        if not Path(LOG_PATH).exists():
            return [], {"parse_errors": 0, "lines_read": 0, "segment_start": 0, "segment_end": 0, "has_prev": False, "has_next": False}
        with open(LOG_PATH, "r", encoding="utf-8", errors="replace") as f:
            raw_lines = f.readlines()
    L = len(raw_lines)
    seg_lines, s0, s1, has_prev, has_next = _physical_segment(raw_lines, args["segment"], args["max_segment_lines"])
    rows = []
    for line in seg_lines:
        row, err = _parse_log_row(line)
        if err == "parse":
            parse_errors += 1
            continue
        if row is None:
            continue
        if _row_matches_filters(row, args):
            rows.append(row)
    rows.sort(key=lambda r: (_row_ts(r) or datetime.datetime.min), reverse=True)
    meta = {
        "lines_read": L,
        "segment_physical_lines": len(seg_lines),
        "parse_errors": parse_errors,
        "segment_start": s0,
        "segment_end": s1,
        "has_prev": has_prev,
        "has_next": has_next,
        "segment": args["segment"],
        "max_segment_lines": args["max_segment_lines"],
    }
    return rows, meta

# -------------------------------------------------------------------------------------------------

http_ignore_api_logs = ["/api/devices", "/api/config"]

@bp.before_request
def _log_http_request_start():
    if request.path in http_ignore_api_logs: return
    g._req_started = time.time()
    log_application(
        "INFO", f"[INFO] {request.method}: {request.path}",
        extra={"component": "http", "http_method": request.method, "path": request.path, "phase": "request"},
    )


@bp.after_request
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


@bp.route("/")
def index():
    return render_template("main.html")

@bp.route('/static/<path:p>')
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

@bp.route("/api/devices", methods=["GET"])
def api_devices():
    devices = read_devices()
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

@bp.route("/api/config", methods=["POST"])
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

@bp.route("/api/remove_mgmt_ip", methods=["POST"])
def api_remove_mgmt_ip():
    body = request.get_json()
    device_id = body.get("device_id")
    if not device_id:
        log_application("WARNING", "[WARNING] api_remove_mgmt_ip: device_id missing", extra={"component": "api"})
        return jsonify({"ok": False, "error": "device_id required"}), 400
    devices = read_devices()
    d = find_device(devices, device_id)
    if not d:
        log_application("WARNING", f"[WARNING] api_remove_mgmt_ip: device not found device_id={device_id}", device_id=device_id, extra={"component": "api"})
        return jsonify({"ok": False, "error": "device not found"}), 404
    old_ip = d.get("mgmt_ip", "")
    update_dynm(device_id, mgmt_ip="")
    log_operation("EDIT", device_id, {"field": "mgmt_ip", "old": old_ip, "new": ""})
    log_application("INFO", f"[INFO] api_remove_mgmt_ip: cleared mgmt_ip (was {old_ip!r})", device_id=device_id, extra={"component": "api", "old_mgmt_ip": old_ip})
    return jsonify({"ok": True})

@bp.route("/api/reserve", methods=["POST"])
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
    devices = read_devices()
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
                    device_id=device_id, user=user,
                    extra={"component": "api", "subsystem": "reservation"},
                )
    
    if ok:
        print(f">>> $ Hostname set to {new_name} for device: {device_id}")
        update_resv(
            device_id,
            tag=d["tag"],
            current_user=d["current_user"],
            duration=d["duration"],
            resv_end_time=d["resv_end_time"],
        )
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


@bp.route("/api/release", methods=["POST"])
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
    devices = read_devices()
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
    update_resv(device_id, tag="free", current_user="", duration="", resv_end_time="")
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
        "INFO", "[INFO] api_release: DB updated tag=free",
        device_id=device_id, user=ru,
        extra={"component": "api", "subsystem": "reservation", "released_user": old_user},
    )
    return jsonify({"ok": ok, "msg": msg})


@bp.route("/api/refresh_health", methods=["POST"])
def api_refresh_health():
    body = request.get_json()
    device_id = body.get("device_id")
    if not device_id:
        log_application("WARNING", "[WARNING] api_refresh_health: device_id missing", extra={"component": "api"})
        return jsonify({"error": "device_id required"}), 400
    devices = read_devices()
    d = find_device(devices, device_id)
    if not d:
        log_application("WARNING", f"[WARNING] api_refresh_health: device not found device_id={device_id}", device_id=device_id, extra={"component": "api"})
        return jsonify({"error": "device not found"}), 404
    log_application("INFO", "[INFO] api_refresh_health: request", device_id=device_id, extra={"component": "api"})

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
            if d:
                update_dynm(device_id, mgmt_ip=ip_addr)
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

@bp.route("/api/db/add", methods=["POST"])
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
    
    devices = read_devices()

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

    new_device = {
        "device_id": body.get("device_id", ""),
        "serial_no": body.get("serial_no", ""),
        "model_name": body.get("model_name", ""),
        "hw_id": body.get("hw_id", ""),
        "mgmt_ip": "",
        "console_ip": console_ip,
        "port_id": str(port_id),
        "tag": "free",
        "current_user": "",
        "duration": "",
        "resv_end_time": "",
    }
    add_device(new_device)

    log_operation("ADD", new_device["device_id"], {"fields": new_device})
    log_application("INFO", f"[INFO] api_db_add: device added device_id={new_device['device_id']}", device_id=new_device["device_id"], extra={"component": "api"})

    # Best-effort mgmt_ip auto-discover
    try:
        if health_manager.ensure_ssh():
            with health_manager.ssh_lock:
                res = telnet_and_run_show_serviceport(health_manager.ssh_client, console_ip, PORT_OFFSET + port_id)
                if res and res.get("interface_status") == "up" and res.get("ip"):
                    ip_addr = res.get("ip")
                    if is_valid_ipv4(ip_addr):
                        update_dynm(new_device["device_id"], mgmt_ip=ip_addr)
                        new_device["mgmt_ip"] = ip_addr
                        log_operation("EDIT", new_device["device_id"], {"field": "mgmt_ip", "old": "", "new": ip_addr, "source": "auto-discover-on-add"})
                        print(f">>> $ Auto-discovered mgmt_ip {ip_addr} for device {new_device['device_id']}")
                        log_application("INFO", f"[INFO] api_db_add: auto-discovered mgmt_ip={ip_addr}", device_id=new_device["device_id"], extra={"component": "api"})
    except Exception as e:
        print(f">>> $ Auto-discover failed for {new_device['device_id']}: {e}")
        log_application("WARNING", f"[WARNING] api_db_add: auto-discover exception: {e}", device_id=new_device["device_id"], extra={"component": "api"})

    return jsonify({"ok": True, "device": new_device})

@bp.route("/api/db/edit", methods=["POST"])
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
    
    devices = read_devices()
    d = find_device(devices, device_id)
    if not d:
        log_application("WARNING", f"[WARNING] api_db_edit: device not found device_id={device_id}", device_id=device_id, extra={"component": "api"})
        return jsonify({"ok": False, "error": "device not found"}), 404

    # Static block is immutable -- silently ignore any attempt to change it (warn-log).
    rejected_static = [f for f in STATIC_FIELDS if f in body and f != "device_id" and body[f] != d.get(f, "")]
    if rejected_static:
        log_application(
            "WARNING",
            f"[WARNING] api_db_edit: ignoring attempt to change static fields {rejected_static}",
            device_id=device_id, extra={"component": "api", "ignored_fields": rejected_static},
        )

    # Only dynm fields are editable through this endpoint.
    editable = ("console_ip", "port_id")
    new_dynm = {}
    changes = {}
    for field in editable:
        if field not in body:
            continue
        new_val = body[field]

        if field == "port_id":
            try:
                pid = int(new_val)
                if pid < 1 or pid > 64:
                    log_application("WARNING", f"[WARNING] api_db_edit: invalid port_id={pid}", device_id=device_id, extra={"component": "api"})
                    return jsonify({"ok": False, "error": "port_id must be between 1-64"}), 400
                new_val = str(pid)
            except (TypeError, ValueError):
                log_application("WARNING", "[WARNING] api_db_edit: port_id not a number", device_id=device_id, extra={"component": "api"})
                return jsonify({"ok": False, "error": "port_id must be a number"}), 400

        if field == "console_ip":
            if not (new_val and is_valid_ipv4(new_val)):
                log_application("WARNING", "[WARNING] api_db_edit: invalid console_ip", device_id=device_id, extra={"component": "api"})
                return jsonify({"ok": False, "error": "missing/invalid console_ip!"}), 400

        old_val = d.get(field, "")
        if old_val != new_val:
            new_dynm[field] = new_val
            changes[field] = {"old": old_val, "new": new_val}

    if not changes:
        log_application("INFO", "[INFO] api_db_edit: no field changes", device_id=device_id, extra={"component": "api"})
        return jsonify({"ok": True, "message": "No changes made"})

    update_dynm(device_id, **new_dynm)
    log_operation("EDIT", device_id, {"fields": changes})
    log_application("INFO", f"[INFO] api_db_edit: saved changes keys={list(changes.keys())}", device_id=device_id, extra={"component": "api"})

    if "port_id" in changes or "console_ip" in changes:
        try:
            if health_manager.ensure_ssh():
                with health_manager.ssh_lock:
                    cip = new_dynm.get("console_ip", d.get("console_ip", ""))
                    pid = int(new_dynm.get("port_id", d.get("port_id", 0)) or 0)
                    res = telnet_and_run_show_serviceport(health_manager.ssh_client, cip, PORT_OFFSET + pid)
                    if res and res.get("interface_status") == "up" and res.get("ip"):
                        ip_addr = res.get("ip")
                        if is_valid_ipv4(ip_addr):
                            old_ip = d.get("mgmt_ip", "")
                            update_dynm(device_id, mgmt_ip=ip_addr)
                            log_operation("EDIT", device_id, {"field": "mgmt_ip", "old": old_ip, "new": ip_addr, "source": "auto-discover-on-port-change"})
                            changes["mgmt_ip"] = {"old": old_ip, "new": ip_addr}
        except Exception as e:
            print(f">>> $ Auto-discover failed after port change for {device_id}: {e}")
            log_application("WARNING", f"[WARNING] api_db_edit: auto-discover after port change failed: {e}", device_id=device_id, extra={"component": "api"})

    return jsonify({"ok": True, "changes": changes})

@bp.route("/api/db/delete", methods=["POST"])
def api_db_delete():
    """Delete device entry from database"""
    body = request.get_json()
    device_id = body.get("device_id")
    if not device_id:
        log_application("WARNING", "[WARNING] api_db_delete: device_id missing", extra={"component": "api"})
        return jsonify({"ok": False, "error": "device_id required"}), 400
    
    snapshot = delete_device(device_id)
    if snapshot is None:
        log_application("WARNING", f"[WARNING] api_db_delete: device not found device_id={device_id}", device_id=device_id, extra={"component": "api"})
        return jsonify({"ok": False, "error": "device not found"}), 404

    log_operation("DELETE", device_id, {"snapshot": snapshot})

    return jsonify({"ok": True})

@bp.route("/api/db/console/ip", methods=["POST"])
def api_db_console_ip():
    """Update console port only (with validation)"""
    body = request.get_json()
    device_id = body.get("device_id")
    console_ip = body.get("console_ip")
    
    if not device_id or console_ip is None:
        return jsonify({"ok": False, "error": "device_id and console_ip required"}), 400
    
    if not (console_ip and is_valid_ipv4(console_ip)):
        return jsonify({"ok": False, "error": "missing/invalid console_ip!"}), 400

    devices = read_devices()
    d = find_device(devices, device_id)
    if not d:
        return jsonify({"ok": False, "error": "device not found"}), 404

    old_ip = d.get("console_ip", "")
    update_dynm(device_id, console_ip=console_ip)

    log_operation("EDIT", device_id, {"field": "console_ip", "old": old_ip, "new": console_ip})
    log_application("INFO", f"[INFO] api_db_console/ip: console_ip set to {console_ip!r}", device_id=device_id, extra={"component": "api"})

    mgmt_discovered = None
    try:
        port_id = int(d.get("port_id", "") or 0)
    except ValueError:
        port_id = 0
    try:
        if port_id and health_manager.ensure_ssh():
            with health_manager.ssh_lock:
                res = telnet_and_run_show_serviceport(health_manager.ssh_client, console_ip, PORT_OFFSET + port_id)
                if res and res.get("interface_status") == "up" and res.get("ip"):
                    ip_addr = res.get("ip")
                    if is_valid_ipv4(ip_addr):
                        old_mgmt = d.get("mgmt_ip", "")
                        update_dynm(device_id, mgmt_ip=ip_addr)
                        log_operation("EDIT", device_id, {"field": "mgmt_ip", "old": old_mgmt, "new": ip_addr, "source": "auto-discover-on-port-change"})
                        mgmt_discovered = ip_addr
    except Exception as e:
        print(f">>> $ Auto-discover failed for {device_id}: {e}")
        log_application("WARNING", f"[WARNING] api_db_console/ip: auto-discover failed: {e}", device_id=device_id, extra={"component": "api"})

    return jsonify({"ok": True, "mgmt_ip": mgmt_discovered})

@bp.route("/api/db/console/port", methods=["POST"])
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
    
    devices = read_devices()
    d = find_device(devices, device_id)
    if not d:
        log_application("WARNING", f"[WARNING] api_db_console/port: device not found device_id={device_id}", device_id=device_id, extra={"component": "api"})
        return jsonify({"ok": False, "error": "device not found"}), 404

    old_port = d.get("port_id", "")
    update_dynm(device_id, port_id=str(port_id))

    log_operation("EDIT", device_id, {"field": "port_id", "old": old_port, "new": str(port_id)})
    log_application("INFO", f"[INFO] api_db_console/port: port_id set to {port_id}", device_id=device_id, extra={"component": "api"})

    mgmt_discovered = None
    console_ip = d.get("console_ip", "")
    try:
        if console_ip and health_manager.ensure_ssh():
            with health_manager.ssh_lock:
                res = telnet_and_run_show_serviceport(health_manager.ssh_client, console_ip, PORT_OFFSET + port_id)
                if res and res.get("interface_status") == "up" and res.get("ip"):
                    ip_addr = res.get("ip")
                    if is_valid_ipv4(ip_addr):
                        old_mgmt = d.get("mgmt_ip", "")
                        update_dynm(device_id, mgmt_ip=ip_addr)
                        log_operation("EDIT", device_id, {"field": "mgmt_ip", "old": old_mgmt, "new": ip_addr, "source": "auto-discover-on-port-change"})
                        mgmt_discovered = ip_addr
    except Exception as e:
        print(f">>> $ Auto-discover failed for {device_id}: {e}")
        log_application("WARNING", f"[WARNING] api_db_console/port: auto-discover failed: {e}", device_id=device_id, extra={"component": "api"})

    return jsonify({"ok": True, "mgmt_ip": mgmt_discovered})

# -------------------------------------------------------------------------------------------------

def _stats_args_from_request():
    preset = (request.args.get("time_preset") or "week").strip().lower()
    if preset not in ("week", "month", "all"):
        preset = "week"
    tf = _time_floor(preset) if preset in ("week", "month") else None
    d = {
        "segment": int(request.args.get("segment", 0) or 0),
        "max_segment_lines": min(int(request.args.get("max_segment_lines", 20000) or 20000), 200000),
        "time_preset": preset,
        "time_floor": tf,
    }
    for key, param in [
        ("device_id", "device_id"),
        ("user", "user"),
        ("operation", "operation"),
        ("component", "component"),
        ("subsystem", "subsystem"),
        ("level", "level"),
        ("path_contains", "path_contains"),
    ]:
        v = (request.args.get(param) or "").strip()
        if v:
            d[key] = v
    return d

def _export_rows(fmt):
    args = _stats_args_from_request()
    rows, _meta = read_and_filter_opslog(args)
    if fmt == "jsonl":
        buf = "\n".join(json.dumps(r, ensure_ascii=False) for r in rows) + ("\n" if rows else "")
        return Response(
            buf,
            mimetype="application/x-ndjson",
            headers={"Content-Disposition": "attachment; filename=operations_export.jsonl"},
        )
    out = io.StringIO()
    w = csv.writer(out)
    w.writerow(
        [
            "timestamp",
            "operation",
            "device_id",
            "user",
            "level",
            "message",
            "component",
            "subsystem",
            "path",
            "status_code",
            "changes_json",
        ]
    )
    for r in rows:
        ch = r.get("changes") or {}
        w.writerow(
            [
                r.get("timestamp", ""),
                r.get("operation", ""),
                r.get("device_id", ""),
                r.get("user", ""),
                ch.get("level", ""),
                (ch.get("message") or "").replace("\n", " ")[:2000],
                ch.get("component", ""),
                ch.get("subsystem", ""),
                ch.get("path", ""),
                ch.get("status_code", ""),
                json.dumps(ch, ensure_ascii=False),
            ]
        )
    return Response(
        out.getvalue(),
        mimetype="text/csv",
        headers={"Content-Disposition": "attachment; filename=operations_export.csv"},
    )


@bp.route("/stats")
def stats_page():
    return render_template("stats.html")

@bp.route("/api/stats/summary", methods=["GET"])
def api_stats_summary():
    args = _stats_args_from_request()
    rows, meta = read_and_filter_opslog(args)
    meta["lines_matched"] = len(rows)
    agg = _aggregate(rows)
    return jsonify({"meta": meta, **agg})

@bp.route("/api/stats/events", methods=["GET"])
def api_stats_events():
    args = _stats_args_from_request()
    rows, meta = read_and_filter_opslog(args)
    offset = max(0, int(request.args.get("events_offset", 0) or 0))
    limit = min(max(1, int(request.args.get("recent_limit", 100) or 100)), 500)
    page = rows[offset : offset + limit]
    meta["events_offset"] = offset
    meta["recent_limit"] = limit
    meta["events_returned"] = len(page)
    meta["has_more_events"] = offset + limit < len(rows)
    meta["lines_matched"] = len(rows)
    return jsonify({"meta": meta, "events": page})

@bp.route("/api/stats/export.jsonl", methods=["GET"])
def api_stats_export_jsonl():
    return _export_rows("jsonl")

@bp.route("/api/stats/export.csv", methods=["GET"])
def api_stats_export_csv():
    return _export_rows("csv")

def register_blueprints(flask_app):
    flask_app.register_blueprint(bp)
