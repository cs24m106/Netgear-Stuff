# app.py
# Minimal Flask backend:
# - reads/writes CSV as the single source of truth (with FileLock)
# - /api/devices -> returns devices + derived reservation fields
# - /api/reserve  -> reserve a free device (sets resv_end_time = now + duration)
# - /api/release  -> release device (tag -> free)
# - /api/events   -> SSE stream of full snapshot every 1s (frontend updates)
# - /api/ping     -> pings the device's mgmt_ip using system ping (used for Health column)

import csv
import json
import os
import time
import subprocess
from datetime import datetime, timedelta, timezone
from flask import Flask, jsonify, request, render_template, Response, stream_with_context
from filelock import FileLock

# CONFIG
CSV_FILE = os.path.join(os.path.dirname(__file__), "database.csv")
CSV_LOCK = CSV_FILE + ".lock"

# EXACT schema you requested
FIELDNAMES = [
    "device_name",
    "model_name",
    "remote_link",
    "mgmt_ip",
    "console_ip",
    "server_port",
    "tag",            # 'resv' or 'free'
    "current_user",
    "resv_end_time"   # ISO 8601 or 'NA' for permanent
]

app = Flask(__name__, static_folder="static", template_folder="templates")


def ensure_csv_exists():
    """Ensure file exists and has header row."""
    if not os.path.exists(CSV_FILE):
        with open(CSV_FILE, "w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=FIELDNAMES)
            writer.writeheader()


def parse_iso_or_na(s):
    """Parse ISO 8601 string to aware UTC datetime; return 'NA' (string) if NA; None if empty/invalid."""
    if not s:
        return None
    s = s.strip()
    if s.upper() == "NA":
        return "NA"
    try:
        dt = datetime.fromisoformat(s)
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)
        return dt.astimezone(timezone.utc)
    except Exception:
        # try trailing Z
        try:
            if s.endswith("Z"):
                return datetime.fromisoformat(s.replace("Z", "+00:00")).astimezone(timezone.utc)
        except Exception:
            return None


def read_devices():
    """
    Read CSV and attach _derived reservation info used by UI.

    Behavior / rules:
    - resv_end_time may be 'NA' (means permanent/static).
    - If resv_end_time is parseable to a datetime:
        * If we can find an explicit duration (resv_duration_min) use it to compute start = end - duration.
        * Otherwise assume start = now and duration = end - now (per your instruction).
    - If values are not applicable (e.g., permanent reservation), we set fields to 'NA' where appropriate.
    - The function produces a human-friendly `display_text`:
        "User: {current_user}, Duration: {HH:MM} . {time_left} left
         Start: {start_iso or NA} End: {end_iso or NA}"
      (time values formatted HH:MM)
    """
    ensure_csv_exists()
    devices = []
    with FileLock(CSV_LOCK):
        with open(CSV_FILE, newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for row in reader:
                devices.append(dict(row))

    now = datetime.now(timezone.utc)

    def secs_to_hhmm(secs):
        """Convert seconds -> 'HH:MM' string, clamps negative to 00:00."""
        if secs is None:
            return "NA"
        s = max(0, int(secs))
        hh = s // 3600
        mm = (s % 3600) // 60
        return f"{hh:02d}:{mm:02d}"

    for d in devices:
        # normalize and read raw end value
        d.setdefault("resv_end_time", (d.get("resv_end_time") or "").strip())
        tag = (d.get("tag") or "").strip().lower()
        end_raw = d.get("resv_end_time") or ""
        parsed_end = parse_iso_or_na(end_raw)  # reused helper: returns datetime, "NA", or None

        # default derived container
        derived = {
            "reserved": False,
            "permanent": False,
            "end_iso": None,
            "remaining_seconds": None,
            "start_iso": None,
            "duration_seconds": None,
            "display_text": ""  # formatted string demanded by user
        }

        if tag == "resv":
            derived["reserved"] = True

            # handle permanent reservation (NA)
            if parsed_end == "NA":
                derived["permanent"] = True
                derived["end_iso"] = "NA"
                derived["remaining_seconds"] = None
                derived["start_iso"] = "NA"
                derived["duration_seconds"] = None

                # Build display text with NA placeholders
                user = d.get("current_user") or "NA"
                derived["display_text"] = (
                    f"User: {user}, Duration: NA . NA left\n"
                    f"Start: NA End: NA"
                )

            # handle parseable end datetime
            elif isinstance(parsed_end, datetime):
                end_dt = parsed_end
                derived["end_iso"] = end_dt.isoformat()

                rem = (end_dt - now).total_seconds()
                derived["remaining_seconds"] = int(rem) if rem > 0 else 0

                duration_seconds = None
                if d.get("resv_duration_min"):
                    try:
                        duration_seconds = int(d.get("resv_duration_min")) * 60
                    except Exception:
                        duration_seconds = None

                # If duration isn't explicitly stored, start = current server read time
                if duration_seconds is None:
                    duration_seconds = int(max(0, (end_dt - now).total_seconds()))
                    start_dt = now 
                else:
                    start_dt = end_dt - timedelta(seconds=duration_seconds)

                derived["duration_seconds"] = int(duration_seconds)
                derived["start_iso"] = start_dt.isoformat()

                # Format textual pieces:
                user = d.get("current_user") or "NA"
                duration_hhmm = secs_to_hhmm(derived["duration_seconds"]) if derived["duration_seconds"] is not None else "NA"
                time_left_hhmm = secs_to_hhmm(derived["remaining_seconds"]) if derived["remaining_seconds"] is not None else "NA"
                start_text = derived["start_iso"] if derived["start_iso"] is not None else "NA"
                end_text = derived["end_iso"] if derived["end_iso"] is not None else "NA"

                # Shorten start/end to human-readable timestamps (ISO is fine; UI may format further)
                derived["display_text"] = (
                    f"User: {user}, Duration: {duration_hhmm} . {time_left_hhmm} left\n"
                    f"Start: {start_text} End: {end_text}"
                )

            else:
                # resv tag but end time is empty/invalid - treat as unknown values
                derived["permanent"] = False
                derived["end_iso"] = None
                derived["remaining_seconds"] = None
                derived["start_iso"] = None
                derived["duration_seconds"] = None

                user = d.get("current_user") or "NA"
                derived["display_text"] = (
                    f"User: {user}, Duration: NA . NA left\n"
                    f"Start: NA End: NA"
                )
        else:
            # not reserved -> present empty / NA display (free entries can reserve)
            derived["reserved"] = False
            derived["permanent"] = False
            derived["end_iso"] = None
            derived["remaining_seconds"] = None
            derived["start_iso"] = None
            derived["duration_seconds"] = None
            derived["display_text"] = "NA"

        d["_derived"] = derived

    return devices


def write_devices(devices):
    """Overwrite CSV with the devices list. Uses FileLock."""
    with FileLock(CSV_LOCK):
        with open(CSV_FILE, "w", newline="", encoding="utf-8") as f:
            writer = csv.DictWriter(f, fieldnames=FIELDNAMES)
            writer.writeheader()
            for d in devices:
                # ensure all fields present
                row = {k: (d.get(k) or "") for k in FIELDNAMES}
                writer.writerow(row)


@app.route("/")
def index():
    return render_template("index.html")


@app.route("/api/devices", methods=["GET"])
def api_devices():
    """Return current devices snapshot (with derived fields)."""
    return jsonify(read_devices())


@app.route("/api/reserve", methods=["POST"])
def api_reserve():
    """
    Reserve a device.
    Request JSON: { device_name, owner, duration_minutes }
    Behavior:
      - If device exists and tag == free: set tag=resv, current_user=owner, resv_end_time = now + duration
      - Return start_iso, end_iso and duration_seconds so UI can show start/duration immediately.
    """
    payload = request.get_json() or {}
    device_name = payload.get("device_name")
    owner = (payload.get("owner") or "").strip()
    try:
        duration_min = int(payload.get("duration_minutes", 0))
    except Exception:
        duration_min = 0

    if not device_name or not owner or duration_min <= 0:
        return jsonify({"ok": False, "error": "device_name, owner and duration_minutes (>0) required"}), 400

    devices = read_devices()
    found = False
    now = datetime.now(timezone.utc)
    end_time = now + timedelta(minutes=duration_min)
    for d in devices:
        if d.get("device_name") == device_name:
            found = True
            if (d.get("tag") or "").strip().lower() == "resv":
                return jsonify({"ok": False, "error": "device already reserved"}), 409
            d["tag"] = "resv"
            d["current_user"] = owner
            d["resv_end_time"] = end_time.isoformat()
            break

    if not found:
        return jsonify({"ok": False, "error": "device not found"}), 404

    write_devices(devices)
    return jsonify({
        "ok": True,
        "start_iso": now.isoformat(),
        "end_iso": end_time.isoformat(),
        "duration_seconds": duration_min * 60
    })


@app.route("/api/release", methods=["POST"])
def api_release():
    """Release a reservation (set tag=free and clear user/end_time)."""
    payload = request.get_json() or {}
    device_name = payload.get("device_name")
    if not device_name:
        return jsonify({"ok": False, "error": "device_name required"}), 400

    devices = read_devices()
    found = False
    for d in devices:
        if d.get("device_name") == device_name:
            found = True
            d["tag"] = "free"
            d["current_user"] = ""
            d["resv_end_time"] = ""
            break

    if not found:
        return jsonify({"ok": False, "error": "device not found"}), 404

    write_devices(devices)
    return jsonify({"ok": True})


@app.route("/api/events")
def api_events():
    """SSE stream: emit full snapshot every 1 second."""
    def gen():
        try:
            while True:
                yield f"data: {json.dumps(read_devices(), default=str)}\n\n"
                time.sleep(1)
        except GeneratorExit:
            return
    return Response(stream_with_context(gen()), mimetype="text/event-stream")


@app.route("/api/ping")
def api_ping():
    """
    Ping endpoint used by frontend to check health.
    Query param: device=<device_name>
    Implementation: uses system 'ping -c 1 -W 1 <ip>' (Linux). Returns JSON { ok: True, up: True/False }.
    NOTE: Using system ping keeps the backend simple for this PoC. On some environments ping may need special permissions.
    """
    device_name = request.args.get("device")
    if not device_name:
        return jsonify({"ok": False, "error": "device query param required"}), 400

    devices = read_devices()
    target = None
    for d in devices:
        if d.get("device_name") == device_name:
            target = d.get("mgmt_ip")
            break
    if not target:
        return jsonify({"ok": False, "error": "device not found"}), 404

    # Run one ping with 1s timeout
    try:
        # -c1 : 1 packet, -W1 : wait time 1 sec (Linux)
        res = subprocess.run(["ping", "-c", "1", "-W", "1", target],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        up = (res.returncode == 0)
    except Exception:
        up = False

    return jsonify({"ok": True, "up": up})


if __name__ == "__main__":
    # development server
    app.run(host="0.0.0.0", port=5000, debug=True)