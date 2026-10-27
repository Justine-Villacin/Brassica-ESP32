"""
desktop.py — Brassica desktop app (local use only, NOT deployed to Vercel).

Run this on your PC to get the dashboard window talking to the physical
ESP32 over your home Wi-Fi:

    python desktop.py              # opens the Brassica window
    python desktop.py --no-window  # no window: just open it in your browser

This file owns every piece that can't run in a serverless environment: the
desktop window, the optional HTTPS server (for the installable-PWA prompt on
phones), the dev-mode file watcher / live reload, and local CSV logging.
Flask itself — routes, templates, static files — lives in app.py.

Install the extra local-only dependencies first:
    pip install -r requirements-desktop.txt
"""

import csv
import os
import socket
import sys
import threading
import time
import webbrowser
from datetime import datetime

# Local desktop mode keeps live-reload ON by default. Set BRASSICA_DEV_MODE=false
# in your environment to turn it off. This MUST happen before `import app`,
# which reads the variable once at import time.
os.environ.setdefault("BRASSICA_DEV_MODE", "true")

try:
    import webview  # pywebview; optional — see the --no-window fallback below
except Exception as _exc:  # noqa: BLE001 - missing package or no GUI backend
    webview = None
    _WEBVIEW_IMPORT_ERROR = _exc
else:
    _WEBVIEW_IMPORT_ERROR = None

from flask import request

from app import BASE_DIR, DEV_MODE, app, broadcast_reload

HTTP_PORT = 8000
HTTPS_PORT = 8443

# ============================================================
# DEV MODE — live reload (no manual refresh, no manual re-run)
# ============================================================
# While DEV_MODE is on, a background thread watches the source files once a
# second:
#   - a .py file changed        -> the whole app restarts itself
#   - a template/static file
#     changed (html/css/js/json) -> every open browser tab reloads itself
PUBLIC_DIR = os.path.join(BASE_DIR, "public")
TEMPLATES_DIR = os.path.join(BASE_DIR, "templates")
WATCH_PATHS = [
    os.path.join(BASE_DIR, "desktop.py"),
    os.path.join(BASE_DIR, "app.py"),
    os.path.join(BASE_DIR, "storage.py"),
    TEMPLATES_DIR,
    PUBLIC_DIR,
]


def _collect_watched_files():
    files = []
    for path in WATCH_PATHS:
        if os.path.isfile(path):
            files.append(path)
        elif os.path.isdir(path):
            for root, _, names in os.walk(path):
                for name in names:
                    files.append(os.path.join(root, name))
    return files


def _snapshot_mtimes():
    snapshot = {}
    for f in _collect_watched_files():
        try:
            snapshot[f] = os.path.getmtime(f)
        except OSError:
            pass
    return snapshot


def watch_for_changes():
    """Polls source files once a second. A Python change restarts the whole
    process; a template/static change just pings connected browsers."""
    last_snapshot = _snapshot_mtimes()
    while True:
        time.sleep(1)
        current = _snapshot_mtimes()
        changed_paths = {
            p for p in set(current) | set(last_snapshot)
            if current.get(p) != last_snapshot.get(p)
        }

        if changed_paths:
            if any(p.endswith(".py") for p in changed_paths):
                print(f"[DevReload] Python change detected: {changed_paths} — restarting Brassica...")
                os.execv(sys.executable, [sys.executable] + sys.argv)
            else:
                print(f"[DevReload] Frontend change detected: {changed_paths} — reloading connected browsers...")
                broadcast_reload()

        last_snapshot = current


# --- Optional HTTPS support (needed for the automatic PWA install prompt) ---
# Drop a cert.pem + key.pem next to this script and a second HTTPS server will
# start alongside the normal HTTP one. Without them HTTPS is simply skipped.
CERT_FILE = os.path.join(BASE_DIR, "cert.pem")
KEY_FILE = os.path.join(BASE_DIR, "key.pem")


def check_pwa_files():
    """Startup diagnostic: prints which required PWA files are missing."""
    required = [
        "manifest.json", "sw.js", "css/main.css", "js/main.js",
        "icons/icon-192.png", "icons/icon-512.png",
    ]
    missing = [f for f in required if not os.path.exists(os.path.join(PUBLIC_DIR, f))]
    if missing:
        print(f"[PWA CHECK] WARNING — missing from {PUBLIC_DIR}: {', '.join(missing)}")
    else:
        print(f"[PWA CHECK] OK — all required PWA files found in {PUBLIC_DIR}")


# ============================================================
# Network helpers — so you always know which IP the ESP32 must use
# ============================================================
def lan_ip_addresses():
    """Best-effort list of this PC's LAN IPv4 addresses (main one first)."""
    found = []

    # The address used for the default route (no packet is actually sent).
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        probe.connect(("10.255.255.255", 1))
        found.append(probe.getsockname()[0])
    except OSError:
        pass
    finally:
        probe.close()

    try:
        for ip in socket.gethostbyname_ex(socket.gethostname())[2]:
            if ip not in found:
                found.append(ip)
    except OSError:
        pass

    return [ip for ip in found if not ip.startswith("127.") and not ip.startswith("169.254.")]


def port_is_free(port):
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        probe.bind(("0.0.0.0", port))
        return True
    except OSError:
        return False
    finally:
        probe.close()


def wait_for_server(port, timeout=10.0):
    """Blocks until something accepts connections on 127.0.0.1:port."""
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return True
        except OSError:
            time.sleep(0.2)
    return False


def print_connection_help():
    ips = lan_ip_addresses()
    print("")
    print("=" * 66)
    print("  Brassica is starting")
    print("=" * 66)
    if ips:
        print(f"  Your PC's address on this network:  {ips[0]}")
        for extra in ips[1:]:
            print(f"      (also seen: {extra})")
        print("")
        print("  The ESP32 must be on the SAME Wi-Fi and point at:")
        print(f"      SERVER_HOST      = \"{ips[0]}\"")
        print(f"      SERVER_PORT      = {HTTP_PORT}")
        print("      SERVER_USE_HTTPS = false")
        print("  (in firmware config.h, or type it in the 'Brassica-Setup'")
        print("   page the ESP32 opens when it cannot connect.)")
    else:
        print("  Could not detect this PC's network address. Is it connected")
        print("  to Wi-Fi/Ethernet? Run `ipconfig` (Windows) or `ifconfig`.")
    print("")
    print(f"  Dashboard on this PC:  http://127.0.0.1:{HTTP_PORT}")
    print("  If the ESP32 never shows up, Windows Firewall is the usual cause:")
    print(f"  allow Python on PRIVATE networks, or allow inbound TCP port {HTTP_PORT}.")
    print("=" * 66)
    print("")


# --- Logs what the ESP32 sends, so connection problems are visible ---
_last_update_status = {"code": None}


@app.after_request
def log_esp32_requests(response):
    if request.path == "/update" and request.method == "POST":
        code = response.status_code
        if code != _last_update_status["code"]:
            _last_update_status["code"] = code
            who = request.remote_addr
            if code == 200:
                print(f"[ESP32] Connected: receiving data from {who}")
            else:
                print(f"[ESP32] {who} sent an update that was REJECTED with HTTP {code}: "
                      f"{response.get_data(as_text=True)[:160]}")
    return response


# ============================================================
# Local CSV logging
# ============================================================
def log_to_csv(temp, hum, soil1, soil2, pump, fan):
    """Logs real-time data or 'ERR' to sensor_data.csv every 2 seconds.
    Local-desktop-only: this file never exists on Vercel."""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    row = [timestamp, temp, hum, soil1, soil2, pump, fan]
    csv_path = os.path.join(BASE_DIR, "sensor_data.csv")

    new_file = not os.path.exists(csv_path) or os.path.getsize(csv_path) == 0
    with open(csv_path, mode="a", newline="", encoding="utf-8") as csv_file:
        writer = csv.writer(csv_file)
        if new_file:
            writer.writerow([
                "Timestamp", "Temperature_C", "Humidity_pct",
                "Soil_1_pct", "Soil_2_pct", "Pump_Active", "Exhaust_Fan_Active",
            ])
        writer.writerow(row)


def background_logging():
    """Background loop exclusively for CSV logging."""
    import storage  # local import keeps desktop-only code out of app.py's import graph

    while True:
        time.sleep(2)
        try:
            state, connected = storage.touch()
            def _num(key, ok):
                v = state.get(key)
                return v if (connected and ok and v is not None) else "ERR"

            temp  = _num("temp",  state.get("dhtConnected"))
            hum   = _num("hum",   state.get("dhtConnected"))
            soil1 = _num("soil",  state.get("soil1Connected"))
            soil2 = _num("soil2", state.get("soil2Connected"))
            
            log_to_csv(
                temp, hum, soil1, soil2,
                state.get("pump", False),
                state.get("fan", False),
            )
        except Exception as exc:  # noqa: BLE001 - never let logging kill the thread
            print(f"[Logger] skipped one log entry: {exc}")

# ============================================================
# Servers
# ============================================================
def run_flask_http():
    """Plain HTTP — used by the ESP32 over Wi-Fi and by the desktop window
    (127.0.0.1 is always treated as secure by browsers)."""
    app.run(host="0.0.0.0", port=HTTP_PORT, debug=False, use_reloader=False, threaded=True)


def run_flask_https():
    """HTTPS — only for phones that want the installable-PWA experience.
    Starts only if cert.pem/key.pem exist next to this file."""
    if os.path.exists(CERT_FILE) and os.path.exists(KEY_FILE):
        print(f"[Brassica] HTTPS enabled — open https://<your-PC-LAN-IP>:{HTTPS_PORT} on your phone to install it.")
        app.run(
            host="0.0.0.0",
            port=HTTPS_PORT,
            debug=False,
            use_reloader=False,
            threaded=True,
            ssl_context=(CERT_FILE, KEY_FILE),
        )
    else:
        print("[Brassica] cert.pem/key.pem not found — HTTPS server not started (the phone install prompt needs it).")


def main():
    use_window = "--no-window" not in sys.argv

    if not port_is_free(HTTP_PORT):
        print(f"\n[Brassica] ERROR: port {HTTP_PORT} is already in use.")
        print("  Another copy of Brassica (or another program) is running on it.")
        print("  Close the old window / stop that program, then run this again.\n")
        sys.exit(1)

    check_pwa_files()
    print_connection_help()

    threading.Thread(target=run_flask_http, daemon=True).start()
    threading.Thread(target=run_flask_https, daemon=True).start()
    threading.Thread(target=background_logging, daemon=True).start()

    if DEV_MODE:
        threading.Thread(target=watch_for_changes, daemon=True).start()
        print("[DevReload] Watching app.py, desktop.py, storage.py, templates/ and public/ for changes...")

    if not wait_for_server(HTTP_PORT):
        print(f"[Brassica] ERROR: the server did not start on port {HTTP_PORT}. See the messages above.")
        sys.exit(1)

    url = f"http://127.0.0.1:{HTTP_PORT}"

    if use_window and webview is not None:
        webview.create_window(
            "Brassica Monitoring",
            url,
            width=1400,
            height=850,
            background_color="#030a07",
        )
        webview.start()
        return

    if use_window and webview is None:
        print(f"[Brassica] Desktop window unavailable ({_WEBVIEW_IMPORT_ERROR}).")
        print("[Brassica] Opening in your browser instead. Run `pip install -r requirements-desktop.txt` to get the window.")
    webbrowser.open(url)
    print("[Brassica] Running. Press Ctrl+C to stop.")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\n[Brassica] Stopped.")


if __name__ == "__main__":
    main()
