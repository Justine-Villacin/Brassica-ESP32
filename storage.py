"""
storage.py — shared state store for Brassica (Serverless & Desktop safe).

History fixes in this version:
  * Only ONE history row is saved per LOG_INTERVAL_SECONDS (default 300 s),
    enforced here on the server, so a chatty ESP32 can no longer fill the
    list and push older days out.
  * The list keeps 14 days of 5-minute rows (MAX_HISTORY), not ~7.
  * No fake/sample rows are ever written. The old fallback that invented
    constant readings (28.5 C / 70 % / 68 % / 73 %) has been removed.
  * A local sensor_data.csv (if one ships with the app) is MERGED into Redis
    once, so past days can be restored. Real rows already in Redis win.
"""

import csv
import json
import os
import threading
import time
from collections import deque
from datetime import datetime

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

def _normalise_redis_url(url):
    if url and url.startswith("redis://") and "upstash.io" in url:
        return "rediss://" + url[len("redis://"):]
    return url


def _env_int(name, default):
    try:
        return int(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


REDIS_URL = _normalise_redis_url(
    (os.environ.get("REDIS_URL") or os.environ.get("KV_URL") or "").strip() or None
)
ON_VERCEL = bool(os.environ.get("VERCEL"))

PREFIX = "brassica:"
KEY_LATEST = PREFIX + "latest"
KEY_HISTORY = PREFIX + "history"
KEY_LOG_GATE = PREFIX + "log_gate"
KEY_CSV_SYNCED = PREFIX + "csv_synced"
KEY_EVENTS = PREFIX + "events"
MAX_EVENTS = 1000

# One row per 5 minutes = 288 rows/day. 4100 rows ~ 14 days.
MAX_HISTORY = _env_int("BRASSICA_MAX_HISTORY", 4100)
LOG_INTERVAL_SECONDS = max(10, _env_int("BRASSICA_LOG_INTERVAL", 300))
# The gate expires a little early so a firmware timer that drifts a few
# seconds never causes a skipped 5-minute row.
_LOG_GATE_SECONDS = max(5, LOG_INTERVAL_SECONDS - 10)

try:
    DISCONNECT_TIMEOUT_SECONDS = float(os.environ.get("BRASSICA_DISCONNECT_TIMEOUT", "35"))
except ValueError:
    DISCONNECT_TIMEOUT_SECONDS = 35.0

DEFAULT_STATE = {
    "esp32Connected": False,
    "dhtConnected": False,
    "soilConnected": False,
    "soil1Connected": False,
    "soil2Connected": False,
    "lightConnected": False,
    "temp": 0.0,
    "hum": 0.0,
    "soil": 0,
    "soil2": 0,
    "light": 0,
    "pump": False,
    "lightState": False,
    "fan": False,
    "fan2": False,
    "wifiSsid": "",
    "ip": "",
    "rssi": 0,
    "firmware": "",
    "_last_update_time": 0,
}


class StorageError(Exception):
    """Redis is configured but could not be reached / used right now."""


# ---------------------------------------------------------------------------
# Redis client (lazy initialization)
# ---------------------------------------------------------------------------

_redis_client = None
_redis_lock = threading.Lock()
_last_error = None
_last_ok_time = 0.0


def _get_client():
    global _redis_client
    if _redis_client is not None:
        return _redis_client
    with _redis_lock:
        if _redis_client is None:
            try:
                import redis
                _redis_client = redis.from_url(
                    REDIS_URL,
                    decode_responses=True,
                    socket_connect_timeout=3,
                    socket_timeout=3,
                    retry_on_timeout=True,
                    health_check_interval=15,
                )
            except Exception as exc:
                raise StorageError(f"could not create Redis client: {exc}") from exc
        return _redis_client


def _reset_client():
    global _redis_client
    with _redis_lock:
        _redis_client = None


def _run(fn):
    global _last_error, _last_ok_time
    last_exc = None
    for attempt in range(2):
        try:
            client = _get_client()
            result = fn(client)
            _last_ok_time = time.time()
            return result
        except StorageError as exc:
            last_exc = exc
        except Exception as exc:
            last_exc = exc
        _reset_client()
    _last_error = (str(last_exc), time.time())
    print(f"[storage] Redis operation failed: {last_exc}")
    raise StorageError(str(last_exc))


# ---------------------------------------------------------------------------
# In-memory store (used when REDIS_URL is NOT configured)
# ---------------------------------------------------------------------------

_mem_lock = threading.Lock()
_mem_latest = dict(DEFAULT_STATE)
_mem_history = deque(maxlen=MAX_HISTORY)
_mem_kv = {}
_mem_last_log = 0.0
_mem_events = deque(maxlen=MAX_EVENTS)

_cache_lock = threading.Lock()
_cache_latest = None


# ---------------------------------------------------------------------------
# Status helpers & state operations
# ---------------------------------------------------------------------------

def is_remote_configured():
    return bool(REDIS_URL)


def backend_name():
    return "redis" if REDIS_URL else "memory"


def last_error():
    return _last_error


def warning():
    if ON_VERCEL and not REDIS_URL:
        return ("Cloud storage is not set up. Add a Redis database in Vercel "
                "(Storage tab) and redeploy, otherwise live data will keep "
                "flickering on and off.")
    if REDIS_URL and _last_error and (time.time() - _last_error[1]) < 60 \
            and _last_ok_time < _last_error[1]:
        return "The Redis database is not responding right now. Retrying..."
    return None


def _decode_hash(raw):
    state = dict(DEFAULT_STATE)
    for field, value in (raw or {}).items():
        try:
            state[field] = json.loads(value)
        except (TypeError, ValueError):
            continue
    return state


def get_latest():
    global _cache_latest
    if REDIS_URL:
        try:
            raw = _run(lambda c: c.hgetall(KEY_LATEST))
            state = _decode_hash(raw)
            with _cache_lock:
                _cache_latest = dict(state)
            return state
        except StorageError:
            with _cache_lock:
                if _cache_latest is not None:
                    return dict(_cache_latest)
            return dict(DEFAULT_STATE)
    with _mem_lock:
        return dict(_mem_latest)


def patch_latest(fields):
    if REDIS_URL:
        mapping = {k: json.dumps(v) for k, v in fields.items()}
        _run(lambda c: c.hset(KEY_LATEST, mapping=mapping))
        return
    with _mem_lock:
        _mem_latest.update(fields)


def _event_dt(ts):
    try:
        return datetime.strptime(str(ts)[:19], "%Y-%m-%d %H:%M:%S")
    except ValueError:
        return None


def _is_duplicate_event(last, event):
    """Same device + same action within 60 s of the newest saved event."""
    if not last:
        return False
    if last.get("type") != event.get("type") or last.get("action") != event.get("action"):
        return False
    a, b = _event_dt(last.get("ts")), _event_dt(event.get("ts"))
    return bool(a and b and abs((b - a).total_seconds()) < 60)


def add_event(event):
    """Saves one activity-log event: {"ts","type","action","msg"}.
    Newest is first in Redis. Near-identical repeats are ignored."""
    if REDIS_URL:
        def op(client):
            last_raw = client.lindex(KEY_EVENTS, 0)
            try:
                last = json.loads(last_raw) if last_raw else None
            except (TypeError, ValueError):
                last = None
            if _is_duplicate_event(last, event):
                return
            pipe = client.pipeline()
            pipe.lpush(KEY_EVENTS, json.dumps(event))
            pipe.ltrim(KEY_EVENTS, 0, MAX_EVENTS - 1)
            pipe.execute()
        _run(op)
        return
    with _mem_lock:
        last = _mem_events[0] if _mem_events else None
        if not _is_duplicate_event(last, event):
            _mem_events.appendleft(event)


def get_events():
    """Activity-log events, oldest first."""
    if REDIS_URL:
        raw_rows = _run(lambda c: c.lrange(KEY_EVENTS, 0, MAX_EVENTS - 1))
        rows = []
        for raw in raw_rows:
            try:
                rows.append(json.loads(raw))
            except (TypeError, ValueError):
                continue
        rows.reverse()
        return rows
    with _mem_lock:
        return list(reversed(_mem_events))


def _auto_events(prev_pump, prev_fan, fields, event_ts, pump_manual, skip_pump=False):
    """ON transitions seen between the previous and the new device state."""
    out = []
    if not event_ts:
        return out
    if not skip_pump and fields.get("pump") is True and not prev_pump:
        out.append({"ts": event_ts, "type": "pump", "action": "on",
                    "msg": "Water Pump turned ON (manual)" if pump_manual
                           else "Water Pump turned ON"})
    if fields.get("fan") is True and not prev_fan:
        out.append({"ts": event_ts, "type": "fan", "action": "on",
                    "msg": "Exhaust Fan turned ON"})
    return out


def record_update(fields, history_row=None, event_ts=None, pump_manual=False, skip_pump_events=False):
    """Saves the live state and, at most once per LOG_INTERVAL_SECONDS,
    one history row. Extra rows offered inside the interval are ignored.
    When event_ts is given, pump/fan OFF->ON changes are also written to the
    activity log, so short runs between history rows are never missed."""
    global _mem_last_log
    if REDIS_URL:
        mapping = {k: json.dumps(v) for k, v in fields.items()}

        def op(client):
            events = []
            if event_ts:
                prev = client.hmget(KEY_LATEST, ["pump", "fan"])
                prev_pump = prev[0] == "true"
                prev_fan = prev[1] == "true"
                events = _auto_events(prev_pump, prev_fan, fields, event_ts, pump_manual, skip_pump_events)

            row = history_row
            if row is not None:
                # SET NX returns True only for the first row in each interval.
                if not client.set(KEY_LOG_GATE, "1", nx=True, ex=_LOG_GATE_SECONDS):
                    row = None

            pipe = client.pipeline()
            pipe.hset(KEY_LATEST, mapping=mapping)
            if row is not None:
                pipe.lpush(KEY_HISTORY, json.dumps(row))
                pipe.ltrim(KEY_HISTORY, 0, MAX_HISTORY - 1)
            pipe.execute()
            return events

        events = _run(op) or []
        for ev in events:
            try:
                add_event(ev)
            except StorageError as exc:
                print(f"[storage] could not save activity event: {exc}")
        return
    with _mem_lock:
        prev_pump = bool(_mem_latest.get("pump"))
        prev_fan = bool(_mem_latest.get("fan"))
        _mem_latest.update(fields)
        if history_row is not None:
            now = time.time()
            if now - _mem_last_log >= _LOG_GATE_SECONDS:
                _mem_last_log = now
                _mem_history.appendleft(history_row)
    for ev in _auto_events(prev_pump, prev_fan, fields, event_ts, pump_manual, skip_pump_events):
        add_event(ev)


# ---------------------------------------------------------------------------
# Restoring past days from a local sensor_data.csv (real data only)
# ---------------------------------------------------------------------------

_csv_checked = False


def _num_or_err(value):
    """CSV cell -> float, or "ERR" for blank / ERR / non-numeric cells."""
    try:
        return float(str(value).replace("°C", "").replace("%", "").strip())
    except (TypeError, ValueError):
        return "ERR"


def _truthy(value):
    return str(value).strip().lower() in ("true", "1", "active", "on")


def _read_local_csv():
    """Returns (rows, signature) from seed_history.py, else sensor_data.csv.
    rows are history dicts; [] if nothing usable."""
    text, sig = None, None
    try:
        import hashlib
        import seed_history  # bundled with the code, so it always deploys
        text = seed_history.csv_text()
        sig = "seed-" + hashlib.md5(text.encode("utf-8")).hexdigest()[:12]
    except Exception:  # noqa: BLE001 - module missing: fall back to the file
        path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sensor_data.csv")
        if not os.path.exists(path):
            return [], None
        try:
            with open(path, "r", encoding="utf-8", newline="") as f:
                text = f.read()
            sig = f"{os.path.getsize(path)}-{int(os.path.getmtime(path))}"
        except Exception as exc:  # noqa: BLE001
            print(f"[storage] Could not read sensor_data.csv: {exc}")
            return [], None

    rows = []
    reader = csv.reader(text.splitlines())
    next(reader, None)  # header
    for row in reader:
        if not row or len(row) < 7 or not str(row[0]).strip():
            continue
        rows.append({
            "timestamp": str(row[0]).split(".")[0].strip(),
            "temp": _num_or_err(row[1]),
            "hum": _num_or_err(row[2]),
            "soil1": _num_or_err(row[3]),
            "soil2": _num_or_err(row[4]),
            "pump": _truthy(row[5]),
            "fan": _truthy(row[6]),
        })
    return rows, sig


def _sync_csv_into_redis(client):
    """Merges sensor_data.csv into the Redis history ONCE per CSV version.
    Rows already in Redis win over CSV rows with the same timestamp."""
    global _csv_checked
    if _csv_checked:
        return
    _csv_checked = True
    try:
        csv_rows, sig = _read_local_csv()
        if not csv_rows or client.get(KEY_CSV_SYNCED) == sig:
            return

        by_ts = {r["timestamp"]: r for r in csv_rows}
        seen_buckets = set()
        # Existing rows are newest first. Keep only one row per 5-minute slot,
        # which also clears the burst of every-2-seconds rows an ESP32 with
        # unplugged sensors can leave behind.
        for raw in client.lrange(KEY_HISTORY, 0, -1):
            try:
                r = json.loads(raw)
            except (TypeError, ValueError):
                continue
            if not (isinstance(r, dict) and r.get("timestamp")):
                continue
            ts = str(r["timestamp"])
            bucket = ts  # fallback if the timestamp is not the usual format
            try:
                bucket = ts[:14] + "%02d" % (int(ts[14:16]) // 5 * 5)
            except ValueError:
                pass
            if bucket in seen_buckets:
                continue
            seen_buckets.add(bucket)
            by_ts[ts] = r  # live row overrides CSV

        merged = sorted(by_ts.values(), key=lambda r: str(r["timestamp"]))[-MAX_HISTORY:]
        # List order in Redis is newest first (LPUSH), so write it reversed.
        payload = [json.dumps(r) for r in reversed(merged)]

        pipe = client.pipeline()
        pipe.delete(KEY_HISTORY)
        pipe.rpush(KEY_HISTORY, *payload)
        pipe.set(KEY_CSV_SYNCED, sig)
        pipe.execute()
        print(f"[storage] Merged sensor_data.csv into Redis: {len(merged)} rows total.")
    except Exception as exc:  # noqa: BLE001
        print(f"[storage] CSV restore error: {exc}")


# ---------------------------------------------------------------------------
# History fetching
# ---------------------------------------------------------------------------

def get_history():
    """History rows, oldest first."""
    if REDIS_URL:
        def _fetch_history(client):
            _sync_csv_into_redis(client)
            return client.lrange(KEY_HISTORY, 0, MAX_HISTORY - 1)

        raw_rows = _run(_fetch_history)
        rows = []
        for raw in raw_rows:
            try:
                rows.append(json.loads(raw))
            except (TypeError, ValueError):
                continue
        rows.reverse()
        return rows
    with _mem_lock:
        return list(reversed(_mem_history))


def touch(now=None):
    now = now if now is not None else time.time()
    state = get_latest()
    last_update = state.get("_last_update_time", 0) or 0
    connected = bool(last_update) and (now - last_update) <= DISCONNECT_TIMEOUT_SECONDS
    return state, connected


# ---------------------------------------------------------------------------
# Key/Value store
# ---------------------------------------------------------------------------

def _kv_key(name):
    return PREFIX + name


def kv_set(name, value, ttl=None):
    data = json.dumps(value)
    if REDIS_URL:
        if ttl:
            _run(lambda c: c.set(_kv_key(name), data, ex=int(ttl)))
        else:
            _run(lambda c: c.set(_kv_key(name), data))
        return
    with _mem_lock:
        _mem_kv[name] = (data, (time.time() + ttl) if ttl else None)


def kv_delete(name):
    if REDIS_URL:
        _run(lambda c: c.delete(_kv_key(name)))
        return
    with _mem_lock:
        _mem_kv.pop(name, None)


def kv_get_many(names):
    names = list(names)
    out = {n: None for n in names}
    if REDIS_URL:
        raws = _run(lambda c: c.mget([_kv_key(n) for n in names]))
        for name, raw in zip(names, raws):
            if raw is not None:
                try:
                    out[name] = json.loads(raw)
                except (TypeError, ValueError):
                    out[name] = None
        return out
    now = time.time()
    with _mem_lock:
        for name in names:
            item = _mem_kv.get(name)
            if not item:
                continue
            data, expires = item
            if expires is not None and expires < now:
                _mem_kv.pop(name, None)
                continue
            out[name] = json.loads(data)
    return out


def kv_get(name):
    return kv_get_many([name])[name]