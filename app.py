"""
app.py — Brassica Flask web app.

This file contains ONLY Flask routes and WSGI-safe imports. Vercel imports
this module directly and calls the `app` object — it never runs the
`if __name__ == "__main__":` block, so anything that needs a real desktop
window, a background thread, or native GUI libraries must NOT live here.

For the pywebview desktop app, the optional HTTPS runner, the dev-mode file
watcher and local CSV logging, see desktop.py (local use only).
"""

import csv
import hmac
import io
import json
import os
import queue
import secrets
import threading
import time
from datetime import datetime, timedelta, timezone

from flask import Flask, Response, jsonify, render_template, request, send_file

import storage
from openpyxl import Workbook

BASE_DIR = os.path.dirname(os.path.abspath(__file__))

# Static assets (css/js/icons/manifest/sw.js) live in public/. Vercel's CDN
# serves that folder directly; pointing Flask's own static_folder at the same
# folder means `python desktop.py` serves the exact same files at the exact
# same URLs (no "/static" prefix).
app = Flask(__name__, static_folder=os.path.join(BASE_DIR, "public"), static_url_path="")
app.json.compact = True  # the ESP32 firmware parses the reply; keep it compact

# ============================================================
# Settings (environment variables)
# ============================================================
DEV_MODE = os.environ.get("BRASSICA_DEV_MODE", "false").strip().lower() in ("1", "true", "yes", "on")

# Optional shared secret required on POST /update and POST /api/control. If
# unset those routes stay open (fine on a home LAN). Strongly recommended
# before pointing a public *.vercel.app URL at real hardware.
API_KEY = os.environ.get("BRASSICA_API_KEY") or None

# Optional PIN that must be typed on the dashboard before the ESP32's Wi-Fi
# network can be changed from the website.
ADMIN_PIN = os.environ.get("BRASSICA_ADMIN_PIN") or None

ON_VERCEL = bool(os.environ.get("VERCEL"))


def _load_timezone():
    """Vercel runs in UTC, which would put 'today' 8 hours off for the
    Philippines. History/exports use this zone instead (change it with
    BRASSICA_TZ, e.g. 'America/New_York')."""
    name = os.environ.get("BRASSICA_TZ", "Asia/Manila")
    try:
        from zoneinfo import ZoneInfo
        return ZoneInfo(name)
    except Exception:  # noqa: BLE001 - tzdata missing or bad name
        print(f"[app] Time zone '{name}' not available, using a fixed offset.")
        return timezone(timedelta(hours=8)) if name == "Asia/Manila" else timezone.utc


LOCAL_TZ = _load_timezone()

_reload_listeners = []
_reload_lock = threading.Lock()


def broadcast_reload():
    """Called by desktop.py's file watcher when a template/static file
    changes, so every connected browser tab reloads itself."""
    with _reload_lock:
        for q in _reload_listeners:
            q.put("reload")


if DEV_MODE:
    @app.route("/dev/reload-stream")
    def dev_reload_stream():
        """Server-Sent-Events stream for the live-reload client in
        index.html. Only registered at all when DEV_MODE is on."""
        def event_stream():
            q = queue.Queue()
            with _reload_lock:
                _reload_listeners.append(q)
            try:
                while True:
                    msg = q.get()
                    yield f"data: {msg}\n\n"
            finally:
                with _reload_lock:
                    if q in _reload_listeners:
                        _reload_listeners.remove(q)

        return Response(event_stream(), mimetype="text/event-stream")


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

@app.after_request
def _no_cache_for_live_data(response):
    """Live data must never be cached by the browser, a proxy or Vercel's
    edge, otherwise the dashboard shows stale 'connected'/'disconnected'."""
    path = request.path
    if path.startswith("/api/") or path in ("/update", "/sensor_data.csv", "/download/excel"):
        response.headers["Cache-Control"] = "no-store, max-age=0"
    return response


@app.errorhandler(storage.StorageError)
def _storage_unavailable(exc):
    return jsonify({
        "status": "error",
        "message": "Storage is unavailable right now. Please retry.",
    }), 503


def _check_api_key(req):
    """True if the request may proceed. With no API key configured every
    request is allowed (dev / home-LAN mode)."""
    if not API_KEY:
        return True
    supplied = req.headers.get("X-API-Key", "")
    return hmac.compare_digest(supplied.encode("utf-8"), API_KEY.encode("utf-8"))


def _check_pin(body):
    if not ADMIN_PIN:
        return True
    supplied = ""
    if isinstance(body, dict):
        supplied = str(body.get("pin") or "")
    return hmac.compare_digest(supplied.encode("utf-8"), ADMIN_PIN.encode("utf-8"))


def _parse_json_body():
    """Parses the request body as a JSON object, tolerating odd bytes (e.g. a
    neighbour's Wi-Fi name with invalid UTF-8 must never break the whole
    sensor update). Returns a dict or None."""
    raw = request.get_data(cache=True)
    try:
        data = json.loads(raw.decode("utf-8", errors="replace"))
    except ValueError:
        return None
    return data if isinstance(data, dict) else None


def _public_state(state):
    """Strips internal bookkeeping fields before a dict is sent to the client."""
    return {k: v for k, v in state.items() if not k.startswith("_")}


def _as_bool(value, default):
    if value is None:
        return default
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() in ("1", "true", "on", "yes")
    return default


def _validate_reading(data):
    """Validates/coerces the sensor numbers in the ESP32 payload. Returns
    (clean_values, errors). Any error rejects the whole request."""
    errors = []
    values = {}

    def bounded_float(key, lo, hi):
        if key not in data:
            return
        raw = data.get(key)
        if isinstance(raw, bool):
            errors.append(f"'{key}' must be a number")
            return
        try:
            val = float(raw)
        except (TypeError, ValueError):
            errors.append(f"'{key}' must be a number")
            return
        if not (lo <= val <= hi):
            errors.append(f"'{key}' must be between {lo} and {hi}")
            return
        values[key] = val

    bounded_float("temperature", -40.0, 80.0)
    bounded_float("humidity", 0.0, 100.0)
    bounded_float("soil_1", 0.0, 100.0)
    bounded_float("soil_2", 0.0, 100.0)
    # "light" is the numeric LDR reading (0-1000). It is deliberately a
    # different key from the grow-light RELAY state ("grow_light": "ON"/"OFF").

    return values, errors


def _short_str(value, limit):
    return value[:limit] if isinstance(value, str) else ""


def _clean_networks(raw):
    """Sanitises the Wi-Fi scan list sent by the ESP32: only real SSIDs,
    de-duplicated (strongest signal wins), strongest first, at most 20."""
    if not isinstance(raw, list):
        return None
    best = {}
    for item in raw:
        if not isinstance(item, dict):
            continue
        ssid = item.get("ssid")
        if not isinstance(ssid, str) or not ssid or len(ssid.encode("utf-8")) > 32:
            continue
        try:
            rssi = int(item.get("rssi", -100))
        except (TypeError, ValueError):
            rssi = -100
        secure = _as_bool(item.get("secure"), True)
        if ssid not in best or rssi > best[ssid]["rssi"]:
            best[ssid] = {"ssid": ssid, "rssi": rssi, "secure": secure}
    return sorted(best.values(), key=lambda n: n["rssi"], reverse=True)[:20]


def _clean_ack(raw):
    """The ESP32 reports the outcome of a Wi-Fi change request here."""
    if not isinstance(raw, dict):
        return None
    ack_id = _short_str(raw.get("id"), 64)
    status = raw.get("status")
    if not ack_id or status not in ("ok", "failed"):
        return None
    return {
        "id": ack_id,
        "status": status,
        "ssid": _short_str(raw.get("ssid"), 32),
        "reason": _short_str(raw.get("reason"), 120),
        "time": time.time(),
    }


def _export_unavailable(message, status=200):
    """A frontend-friendly JSON response for the CSV/Excel export routes,
    used instead of a bare text 404. downloadFile() in public/js/utils.js
    shows it as a toast in the activity feed."""
    return jsonify({"status": "unavailable", "message": message}), status


def _local_csv_path():
    """Desktop-mode CSV log. Only ever exists when running locally via
    desktop.py — never on Vercel's read-only/ephemeral filesystem."""
    return os.path.join(BASE_DIR, "sensor_data.csv")


# ---------------------------------------------------------------------------
# Page + status routes
# ---------------------------------------------------------------------------

@app.route("/")
def index():
    return render_template("index.html", dev_mode=DEV_MODE)


@app.route("/api/sensors")
def api_sensors():
    state, connected = storage.touch()
    out = _public_state(state)

    last_update = state.get("_last_update_time", 0) or 0
    out["esp32Age"] = round(time.time() - last_update, 1) if last_update else None
    out["storage"] = storage.backend_name()
    out["storageWarning"] = storage.warning()

    if not connected:
        # The ESP32 hasn't been heard from recently, so we genuinely don't
        # know the real state of the relays: never show them as ACTIVE.
        
        out["esp32Connected"] = False
        out["dhtConnected"] = False
        out["soilConnected"] = False
        out["soil1Connected"] = False     
        out["soil2Connected"] = False
        out["lightConnected"] = False
        out["fan"] = False
        out["pump"] = False
        out["lightState"] = False
        out["temp"]  = None
        out["hum"]   = None
        out["soil"]  = None
        out["soil2"] = None
    override = storage.kv_get_many(["manual_pump"])["manual_pump"]
    out["manualPumpActive"] = override is not None
    out["manualPumpOn"] = bool(override.get("state")) if override else False
    return jsonify(out)


@app.route("/api/status")
def api_status():
    """Diagnostic endpoint: open it in a browser right after a deploy to see
    whether Redis is wired up and whether the ESP32 is reporting in."""
    _, connected = storage.touch()
    err = storage.last_error()
    return jsonify({
        "app": "Brassica",
        "storage_backend": storage.backend_name(),
        "redis_configured": storage.is_remote_configured(),
        "storage_warning": storage.warning(),
        "last_storage_error": err[0] if err else None,
        "dev_mode": DEV_MODE,
        "api_key_required": bool(API_KEY),
        "admin_pin_required": bool(ADMIN_PIN),
        "esp32_connected": connected,
        "disconnect_timeout_seconds": storage.DISCONNECT_TIMEOUT_SECONDS,
    })


@app.route("/api/control", methods=["POST"])
def api_control():
    if not _check_api_key(request):
        return jsonify({"status": "error", "message": "unauthorized"}), 401

    data = _parse_json_body()
    if data is None:
        return jsonify({"status": "error", "message": "invalid or missing JSON body"}), 400

    target = data.get("target")
    if target != "pump":
        return jsonify({"status": "error", "message": f"unsupported target '{target}'"}), 400

    state = _as_bool(data.get("state"), False)

    # Store the override so /update can echo it back to the ESP32 on every
    # post. 180 s TTL: the ESP32 re-confirms roughly every 10 s while the
    # override is active (it sends "ctl": true on every post), so the key
    # stays alive while you're using it and expires ~3 min after you stop.
    storage.kv_set("manual_pump", {"state": state, "set": time.time()}, ttl=180)

    # Also update the cache immediately so the dashboard's next poll shows
    # the button in the right state without waiting for the ESP32 round-trip.
    storage.patch_latest({"pump": state})

    return jsonify({
        "status": "success",
        "manual_pump": state,
        "sensor_data": _public_state(storage.get_latest()),
    })

# --- receives live JSON from the physical ESP32 ---
@app.route("/update", methods=["POST"])
def update_from_esp32():
    if not _check_api_key(request):
        return jsonify({"status": "error", "message": "unauthorized"}), 401

    data = _parse_json_body()
    if data is None:
        print("[update] rejected, body is not a JSON object:",
              request.get_data(as_text=True)[:200])
        return jsonify({"status": "error", "message": "invalid or missing JSON body"}), 400

    values, errors = _validate_reading(data)
    if errors:
        return jsonify({"status": "error", "message": "; ".join(errors)}), 400

    now = time.time()

    dht_ok = _as_bool(data.get("dht_connected"), True)
    soil1_ok = _as_bool(data.get("soil1_connected"), True)
    soil2_ok = _as_bool(data.get("soil2_connected"), True)
    soil_ok = soil1_ok or soil2_ok

    # "ON"/"OFF" strings from the firmware -> booleans. Defense in depth: a
    # device must never be reported ON while the sensor it depends on is
    # disconnected (the firmware already forces it OFF; enforce it here too).
    fan = (data.get("fan") == "ON") and dht_ok
    fan2 = (data.get("fan2") == "ON") and dht_ok
    pump = (data.get("water_pump") == "ON") and soil_ok


    fields = {
        "esp32Connected": True,
        "dhtConnected": dht_ok,
        "soilConnected": soil_ok,
        "soil1Connected": soil1_ok,     
        "soil2Connected": soil2_ok,
        "fan": fan,
        "fan2": fan2,
        "pump": pump,
        "_last_update_time": now,
    }
    fields["temp"]  = values.get("temperature") if dht_ok else None
    fields["hum"]   = values.get("humidity")    if dht_ok else None
    fields["soil"]  = values.get("soil_1")      if soil1_ok else None
    fields["soil2"] = values.get("soil_2")      if soil2_ok else None


    # Optional device info (shown in the dashboard's Wi-Fi panel).
    if isinstance(data.get("wifi_ssid"), str):
        fields["wifiSsid"] = _short_str(data["wifi_ssid"], 32)
    if isinstance(data.get("ip"), str):
        fields["ip"] = _short_str(data["ip"], 45)
    try:
        if "rssi" in data and not isinstance(data["rssi"], bool):
            fields["rssi"] = int(data["rssi"])
    except (TypeError, ValueError):
        pass
    if isinstance(data.get("fw"), str):
        fields["firmware"] = _short_str(data["fw"], 16)

    # The firmware sets "log": true about every 5 minutes. Saving a history
    # row on every post would use up a free Redis plan within days.
    # The firmware sets "log": true about every 5 minutes. Saving a history
    # row on every post would use up a free Redis plan within days.
    history_row = None
    if _as_bool(data.get("log"), True):
        # Fallback to the last known live dashboard state if ESP32 omits the reading
        latest_state = storage.get_latest() or {}

        soil1_val = values.get("soil_1") if "soil_1" in values else data.get("soil_1")
        if soil1_val is None:
            soil1_val = latest_state.get("soil1")

        soil2_val = values.get("soil_2") if "soil_2" in values else data.get("soil_2")
        if soil2_val is None:
            soil2_val = latest_state.get("soil2")

        try:
            soil1_num = float(soil1_val) if soil1_val is not None else "ERR"
        except (TypeError, ValueError):
            soil1_num = "ERR"

        try:
            soil2_num = float(soil2_val) if soil2_val is not None else "ERR"
        except (TypeError, ValueError):
            soil2_num = "ERR"

        history_row = {
            "timestamp": datetime.fromtimestamp(now, LOCAL_TZ).strftime("%Y-%m-%d %H:%M:%S"),
            "temp": values.get("temperature", "ERR") if dht_ok else "ERR",
            "hum": values.get("humidity", "ERR") if dht_ok else "ERR",
            "soil1": soil1_num,
            "soil2": soil2_num,
            "pump": pump,
            "fan": fan,
        }
    storage.record_update(fields, history_row)  # raises StorageError -> HTTP 503

    # ----- Everything below is best-effort: the reading is already saved, so a
    # hiccup here must never make the ESP32 think its update failed. -----
    # ----- Manual pump override -----
    # If the user pressed "Turn ON/OFF" on the dashboard, /api/control has
    # stored it in Redis under "manual_pump". Echo it back to the ESP32 so
    # the firmware latches the pump to that state (see
    # MANUAL_OVERRIDE_TIMEOUT_MS in config.h). Refresh the TTL by rewriting
    # the value — the firmware confirms on every post while the override is
    # active ("ctl": true), so this fires roughly every 10 s hosted / 3 s local.
    latest_state = storage.get_latest()
    override = storage.kv_get_many(["manual_pump"])["manual_pump"]
    if override is not None:
        try:
            storage.kv_set("manual_pump", override, ttl=180)   # refresh TTL
        except storage.StorageError:
            pass
        reply = {
            "status": "success",
            "manual_pump": bool(override.get("state")),
        }
    else:
        reply = {"status": "success"}
    try:
        networks = _clean_networks(data.get("wifi_networks"))
        if networks is not None:
            storage.kv_set("wifi_networks", {"list": networks, "time": now}, ttl=86400)

        ack = _clean_ack(data.get("wifi_ack"))
        # The ESP32 asks for pending commands about every 30 s ("ctl": true),
        # and always when it is reporting the result of a Wi-Fi change. Reading
        # them on every post would triple the database traffic.
        if ack or _as_bool(data.get("ctl"), False):
            ctl = storage.kv_get_many(["wifi_pending", "wifi_scan_req"])
            pending = ctl["wifi_pending"]

            if ack:
                storage.kv_set("wifi_result", ack, ttl=3600)
                if pending and pending.get("id") == ack["id"]:
                    storage.kv_delete("wifi_pending")
                    pending = None

            if pending:
                # Hex-encoded so no character in an SSID/password can break JSON.
                reply["wifi_config"] = {
                    "id": pending["id"],
                    "ssid_hex": pending["ssid"].encode("utf-8").hex(),
                    "pass_hex": pending["password"].encode("utf-8").hex(),
                }
            if ctl["wifi_scan_req"]:
                reply["scan"] = True
                storage.kv_delete("wifi_scan_req")
    except storage.StorageError:
        pass
    except Exception as exc:  # noqa: BLE001
        print(f"[update] post-save step failed (ignored): {exc}")

    return jsonify(reply)
# ---------------------------------------------------------------------------
# Wi-Fi setup from the dashboard
# ---------------------------------------------------------------------------
# Flow: the website stores a short-lived "pending" Wi-Fi request in Redis ->
# the ESP32 receives it in the reply to its next POST /update -> it tries the
# new network (and falls back to the old one if that fails) -> it reports the
# result back in a later POST /update -> the website shows it.

@app.route("/api/wifi/scan")
def api_wifi_scan():
    ctl = storage.kv_get_many(["wifi_networks", "wifi_scan_req"])
    found = ctl["wifi_networks"] or {}
    scanned_at = found.get("time")
    return jsonify({
        "status": "success",
        "networks": found.get("list", []),
        "age": round(time.time() - scanned_at, 1) if scanned_at else None,
        "scanning": bool(ctl["wifi_scan_req"]),
    })


@app.route("/api/wifi/rescan", methods=["POST"])
def api_wifi_rescan():
    """Asks the ESP32 to scan again; it sees the request on its next post."""
    storage.kv_set("wifi_scan_req", True, ttl=120)
    return jsonify({"status": "success"})


@app.route("/api/wifi/status")
def api_wifi_status():
    ctl = storage.kv_get_many(["wifi_pending", "wifi_result"])
    state, connected = storage.touch()
    pending = ctl["wifi_pending"]
    return jsonify({
        "status": "success",
        "pin_required": bool(ADMIN_PIN),
        "open_access_warning": ON_VERCEL and not ADMIN_PIN,
        "device": {
            "connected": connected,
            "ssid": state.get("wifiSsid", ""),
            "ip": state.get("ip", ""),
            "rssi": state.get("rssi", 0),
            "firmware": state.get("firmware", ""),
        },
        "pending": ({"id": pending["id"], "ssid": pending["ssid"],
                     "created": pending.get("created")} if pending else None),
        "result": ctl["wifi_result"],
    })


@app.route("/api/wifi/configure", methods=["POST"])
def api_wifi_configure():
    body = _parse_json_body()
    if body is None:
        return jsonify({"status": "error", "message": "invalid or missing JSON body"}), 400
    if not _check_pin(body):
        return jsonify({"status": "error", "message": "Wrong PIN."}), 401

    ssid = body.get("ssid")
    password = body.get("password", "")
    if not isinstance(ssid, str) or not isinstance(password, str):
        return jsonify({"status": "error", "message": "Network name and password must be text."}), 400
    if not ssid or len(ssid.encode("utf-8")) > 32:
        return jsonify({"status": "error", "message": "The network name must be 1-32 characters."}), 400
    if password and not (8 <= len(password.encode("utf-8")) <= 63):
        return jsonify({"status": "error",
                        "message": "Wi-Fi passwords are 8-63 characters (leave it empty for an open network)."}), 400

    pending = {
        "id": secrets.token_hex(6),
        "ssid": ssid,
        "password": password,
        "created": time.time(),
    }
    storage.kv_set("wifi_pending", pending, ttl=600)   # auto-expires, password never lingers
    storage.kv_delete("wifi_result")

    _, connected = storage.touch()
    return jsonify({
        "status": "success",
        "id": pending["id"],
        "device_online": connected,
        "message": ("Sent. The ESP32 picks this up within about 30 seconds."
                    if connected else
                    "Saved, but the ESP32 is offline. It will apply this the next time it checks in "
                    "(requests expire after 10 minutes)."),
    })


# ---------------------------------------------------------------------------
# Data export (CSV / Excel) and daily averages
# ---------------------------------------------------------------------------
# History source of truth, in priority order:
#   1. The local desktop CSV written by desktop.py (only exists locally).
#   2. Redis (hosted), or the in-memory store (local, no Redis).
#   3. Nothing yet -> a friendly "not available yet" JSON response.
CSV_HEADERS = [
    "Timestamp", "Temperature_C", "Humidity_pct", "Soil_1_pct", "Soil_2_pct",
    "Pump_Active", "Exhaust_Fan_Active",
]

def _history_rows_from_store():
    rows = storage.get_history()
    return [
        [r.get("timestamp"), r.get("temp"), r.get("hum"), 
         r.get("soil1"), r.get("soil2"), r.get("pump"), r.get("fan")]
        for r in rows
    ]


def _history_rows_from_local_csv():
    path = _local_csv_path()
    if not os.path.exists(path):
        return None
    rows = []
    with open(path, "r", encoding="utf-8", newline="") as f:
        reader = csv.reader(f)
        next(reader, None)
        for row in reader:
            if not row:
                continue
            # Ensure row matches canonical length, padding if necessary
            while len(row) < len(CSV_HEADERS):
                row.append("")
            rows.append(row[:len(CSV_HEADERS)])
    return rows

def _resolve_history_rows():
    """Returns (rows, source); source is None if there is nothing to export."""
    # On Vercel, NEVER check local CSV files — go straight to cloud Redis/store.
    if not ON_VERCEL:
        local_rows = _history_rows_from_local_csv()
        if local_rows is not None:
            return local_rows, "local"

    rows = _history_rows_from_store()
    if rows:
        return rows, storage.backend_name()

    if storage.is_remote_configured():
        # Redis is wired up but still empty (a brand-new deployment).
        return [], storage.backend_name()

    return None, None


@app.route("/sensor_data.csv")
def download_csv_file():
    rows, source = _resolve_history_rows()
    if source is None:
        return _export_unavailable(
            "No sensor history is available yet. It appears once your ESP32 "
            "has sent at least one reading."
        )

    buffer = io.StringIO()
    writer = csv.writer(buffer)
    writer.writerow(CSV_HEADERS)
    writer.writerows(rows)

    mem = io.BytesIO(buffer.getvalue().encode("utf-8"))
    return send_file(
        mem,
        as_attachment=True,
        download_name="Brassica_Complete_Data.csv",
        mimetype="text/csv",
    )


@app.route("/download/excel")
def download_excel_file():
    rows, source = _resolve_history_rows()
    if source is None:
        return _export_unavailable(
            "No sensor history is available yet, so there is nothing to "
            "export. It appears once your ESP32 has sent readings."
        )

    wb = Workbook()
    wb.remove(wb.active)  # remove the default empty sheet

    days_data = {}
    for row in rows:
        if not row or str(row[0]).startswith("---"):
            continue
        date_str = str(row[0]).split(" ")[0][:31]
        days_data.setdefault(date_str, []).append(row)

    if days_data:
        for date_str, day_rows in days_data.items():
            ws = wb.create_sheet(title=date_str)
            ws.append(CSV_HEADERS)
            for r in day_rows:
                ws.append(r)
    else:
        ws = wb.create_sheet(title="Data")
        ws.append(CSV_HEADERS)

    mem = io.BytesIO()
    wb.save(mem)
    mem.seek(0)

    return send_file(
        mem,
        as_attachment=True,
        download_name="Brassica_Data.xlsx",
        mimetype="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    )
@app.route("/api/history")
def api_history():
    """Daily averages for the last 7 days that have data."""
    rows, _source = _resolve_history_rows()
    if not rows:
        return jsonify({"status": "success", "history": []})

    daily = {}
    for r in rows:
        if not r or len(r) < 5:
            continue
        
        t_val, h_val, s1_val, s2_val = r[1], r[2], r[3], r[4]
        
        def clean_num(val):
            if val is None or val == "ERR" or val == "Disabled" or val == "Inactive" or val == "":
                return None
            if isinstance(val, (int, float)):
                return float(val)
            cleaned = str(val).replace("°C", "").replace("°", "").replace("%", "").strip()
            try:
                return float(cleaned)
            except (ValueError, TypeError):
                return None

        t_num = clean_num(t_val)
        h_num = clean_num(h_val)
        s1_num = clean_num(s1_val)
        s2_num = clean_num(s2_val)

        # Robust date parsing
        raw_date_str = str(r[0]).split(".")[0].strip()
        dt = None
        for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
            try:
                dt = datetime.strptime(raw_date_str, fmt)
                break
            except ValueError:
                continue
        
        if not dt:
            continue

        bucket = daily.setdefault(dt.date(), {"t": [], "h": [], "s1": [], "s2": []})
        if t_num is not None: bucket["t"].append(t_num)
        if h_num is not None: bucket["h"].append(h_num)
        if s1_num is not None: bucket["s1"].append(s1_num)
        if s2_num is not None: bucket["s2"].append(s2_num)

    # Sort dates chronologically and take the most recent 7 days
    sorted_days = sorted(daily.keys())
    recent_days = sorted_days[-7:]

    history = []
    for day in recent_days:
        b = daily[day]
        history.append({
            "date": day.strftime("%b %d"),
            "temp": round(sum(b["t"]) / len(b["t"]), 1) if b["t"] else "--",
            "hum": round(sum(b["h"]) / len(b["h"]), 1) if b["h"] else "--",
            "soil1": round(sum(b["s1"]) / len(b["s1"]), 1) if b["s1"] else "--",
            "soil2": round(sum(b["s2"]) / len(b["s2"]), 1) if b["s2"] else "--",
        })

    return jsonify({"status": "success", "history": history})

@app.route("/api/activity")
def api_activity():
    """Returns automation turn-ON events for a specific date, plus available dates."""
    rows, _source = _resolve_history_rows()
    if not rows:
        return jsonify({"status": "success", "events": [], "dates": [], "current_date": None})

    # Group events by date and collect all unique valid dates
    days_map = {}
    prev_pump = False
    prev_fan = False
    
    for r in rows:
        if not r or len(r) < 6:
            continue
        
        ts_str = r[0]
        if len(r) >= 7:
            pump_val, fan_val = r[5], r[6]
        else:
            pump_val, fan_val = r[4], r[5]
        
        is_pump = str(pump_val).strip().lower() in ("true", "1", "active", "on")
        is_fan = str(fan_val).strip().lower() in ("true", "1", "active", "on")

        # Parse date safely
        raw_date_str = str(ts_str).split(".")[0].strip()
        dt = None
        for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y-%m-%d"):
            try:
                dt = datetime.strptime(raw_date_str, fmt)
                break
            except ValueError:
                continue
        
        if not dt:
            continue

        date_key = dt.strftime("%Y-%m-%d")
        day_events = days_map.setdefault(date_key, [])

        # Detect transition from OFF -> ON
        if is_pump and not prev_pump:
            day_events.append({"time": dt.strftime("%I:%M %p"), "msg": "Water Pump turned ON", "type": "pump"})
        if is_fan and not prev_fan:
            day_events.append({"time": dt.strftime("%I:%M %p"), "msg": "Exhaust Fan turned ON", "type": "fan"})

        prev_pump = is_pump
        prev_fan = is_fan

    available_dates = sorted([d for d in days_map.keys() if days_map[d]], reverse=True)
    if not available_dates:
        return jsonify({"status": "success", "events": [], "dates": [], "current_date": None})

    # Requested date or default to the latest date with events
    requested_date = request.args.get("date")
    if not requested_date or requested_date not in days_map:
        current_date = available_dates[0]
    else:
        current_date = requested_date

    events = days_map.get(current_date, [])
    events.reverse()  # Newest first for that day

    return jsonify({
        "status": "success",
        "events": events,
        "dates": available_dates,
        "current_date": current_date
    })