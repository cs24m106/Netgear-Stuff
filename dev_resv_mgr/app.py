"""Application module: Flask app, CLI."""
import argparse
import subprocess
from flask import Flask

import config
from config import *
import utils
import api


# -----------------------
# Flask application
# -----------------------
app = Flask(__name__, static_folder="static", template_folder="templates")
api.register_blueprints(app)


def start_background_services():
    utils.init_device_state_from_db()
    utils.health_manager.start()
    utils.reservation_monitor.start()
    utils.log_application(
        "INFO",
        "[INFO] background service threads started",
        extra={"component": "startup"},
    )


def kill_process_on_port(port):
    """Forcefully kills any process running on the specified port (Linux/Unix)."""
    try:
        result = subprocess.check_output(["lsof", "-t", f"-i:{port}"])
        pids = result.decode().strip().split("\n")
        for pid in pids:
            if pid:
                print(f">>> $ Forcefully clearing port {port} (Killing PID {pid})...")
                subprocess.run(["kill", "-9", pid])
    except subprocess.CalledProcessError:
        pass
    except Exception as e:
        print(f">>> $ Note: Could not auto-kill process on port {port}: {e}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Switch Reservation UI")
    parser.add_argument("--debug", action="store_true", default=False, help="Enable Flask debug mode")
    parser.add_argument("-p", "--port", type=int, default=7000, help="Port number to run the server on")
    args = parser.parse_args()

    kill_process_on_port(args.port)
    config.FLASK_DEBUG = bool(args.debug)
    app.debug = config.FLASK_DEBUG
    start_background_services()
    utils.log_application(
        "INFO",
        f"[INFO] Flask listen host=0.0.0.0 port={args.port} debug={args.debug}",
        extra={"component": "startup", "port": args.port, "debug": args.debug},
    )
    app.run(host="0.0.0.0", port=args.port, debug=args.debug, use_reloader=False, threaded=True)
