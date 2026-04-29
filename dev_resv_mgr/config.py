"""Global configuration: constants, paths, lab credentials, shared backend state.

Loaded with: ``from config import *``

Lab SSH credentials (PI / NUC) are read at import time from the ``lab`` block
of ``database.json`` so we don't keep secrets in source.
"""

import ipaddress
import json
import threading
from pathlib import Path

# -------------------------------------------------------------------------------------------------
# Paths
# -------------------------------------------------------------------------------------------------
DB_PATH = "database.json"
DB_LOCK = DB_PATH + ".lock"
LOG_PATH = "operations.log"
LOG_LOCK = LOG_PATH + ".lock"

# -------------------------------------------------------------------------------------------------
# Switch / network constants
# -------------------------------------------------------------------------------------------------
PORT_OFFSET = 10000
SWITCH_USER = "admin"
SWITCH_PASSWORD = "Netgear@@123"

AV_UI = {"port": 4443, "offset": 60000}
MAIN_UI = {"port": 49152, "offset": 51000}
MAIN_UI_OLD = {"port": 49151, "offset": 50000}

UP_HEALTH_TIMER = 10        # seconds
DOWN_HEALTH_TIMER = 2       # seconds
MAX_HEALTH_CHECK_RETRIES = 3

# Fields a user is allowed to edit through the DB-edit API (always 'static' fields
# plus the dynm transport fields console_ip / port_id).
UPDATABLE_FIELDS = ["device_id", "serial_no", "model_name", "hw_id", "console_ip", "port_id"]

# Field membership per nested group in database.json (single source of truth).
STATIC_FIELDS = ("device_id", "serial_no", "model_name", "hw_id")
DYNM_FIELDS = ("mgmt_ip", "console_ip", "port_id")
RESV_FIELDS = ("tag", "current_user", "duration", "resv_end_time")
ALL_FIELDS = STATIC_FIELDS + DYNM_FIELDS + RESV_FIELDS

# -------------------------------------------------------------------------------------------------
# Lab credentials -- read from database.json {"lab": {"PI": {...}, "NUC": {...}}}
# -------------------------------------------------------------------------------------------------
def _load_lab_creds():
    """Return the ``lab`` block from DB_PATH or {} if not present / unreadable."""
    p = Path(DB_PATH)
    if not p.exists():
        return {}
    try:
        with open(p, "r", encoding="utf-8") as f:
            return (json.load(f) or {}).get("lab", {}) or {}
    except Exception:
        return {}

_LAB = _load_lab_creds()
#_PI = _LAB.get("PI", {}) or {}
_NUC = _LAB.get("NUC", {}) or {}

# Default SSH session used = NUC.
SSH_IP = _NUC.get("IP")
SSH_USER = _NUC.get("USER")
SSH_PSWD = _NUC.get("PSWD")

# -------------------------------------------------------------------------------------------------
# Shared runtime state (was in utils.py)
# -------------------------------------------------------------------------------------------------
device_state = {}
device_state_lock = threading.Lock()

# Flask debug flag -- set by app.py after the Flask() instance is created.
# Other modules just read ``config.FLASK_DEBUG`` instead of importing app.
FLASK_DEBUG = False


def is_valid_ipv4(addr):
    try:
        ipaddress.IPv4Address(addr)
        return True
    except Exception:
        return False


def find_device(devices, device_id):
    for d in devices:
        if d.get("device_id") == device_id:
            return d
    return None


# -------------------------------------------------------------------------------------------------
# Logging shims -- avoid utils <-> app circular import; utils holds implementations.
# -------------------------------------------------------------------------------------------------
def log_app(level, message, device_id="-", user="system", **extra):
    import utils as _m
    return _m.log_application(level, message, device_id=device_id, user=user, **extra)


def log_op(operation, device_id, changes, user="system"):
    import utils as _m
    return _m.log_operation(operation, device_id, changes, user=user)
