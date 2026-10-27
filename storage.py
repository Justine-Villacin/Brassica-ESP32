"""
storage.py — shared state store for Brassica.

Why this exists
---------------
On Vercel, app.py runs as a serverless function: every request can land on a
different instance with no memory of the last one, so readings must live in
an external store (Redis). This module gives app.py one small surface:

  * REDIS_URL (or KV_URL) set  -> everything is read/written through Redis.
  * nothing set                -> a plain in-memory store, which is perfect
                                  for `python desktop.py` on your PC.

v6.0 changes (these fix the "dashboard keeps going on/off" problem):

  * When Redis IS configured, the app never silently switches to a private
    per-instance memory dict any more. The old code cached "Redis failed"
    forever on the instance that hit one slow ping, so different instances
    ended up showing different data and the dashboard flickered. Now a
    failure is reported (HTTP 503 to the ESP32, which simply retries) and the
    connection is retried on the next request.
  * Reads fall back to the last good copy held by *this* instance if Redis
    has a short hiccup, so one bad request cannot blank the dashboard.
  * Live state is a Redis hash that is updated with ONE atomic command, so
    the dashboard, the ESP32 and the Wi-Fi setup routes can no longer
    overwrite each other's data (the old read-modify-write lost updates).
  * The ESP32 "disconnected" timeout is 35 s (was 8 s). On Vercel the
    firmware posts every 10 s and a cold start + TLS handshake can take
    several seconds, so 8 s flagged healthy devices as offline.
  * Redis traffic is kept deliberately small. The free Upstash plan that
    Vercel offers allows 500,000 commands a month; once it is used up the
    database refuses requests and the dashboard would flicker for the rest of
    the month. An ESP32 post costs ONE command (plus a history row every
    5 minutes), and a dashboard refresh costs one.
"""

import json
import os
import threading
import time
from collections import deque
import pandas as pd

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------


def _normalise_redis_url(url):
    """Upstash (the Redis behind Vercel's Marketplace) only accepts TLS. If a
    plain redis:// URL for an upstash.io host is pasted in, upgrade it."""
    if url and url.startswith("redis://") and "upstash.io" in url:
        return "rediss://" + url[len("redis://"):]
    return url


REDIS_URL = _normalise_redis_url(
    (os.environ.get("REDIS_URL") or os.environ.get("KV_URL") or "").strip() or None
)
ON_VERCEL = bool(os.environ.get("VERCEL"))

PREFIX = "brassica:"
KEY_LATEST = PREFIX + "latest"      # Redis hash: field -> JSON value
KEY_HISTORY = PREFIX + "history"    # Redis list, newest first
MAX_HISTORY = 2100                  # capped list length: 7 days of 5-minute samples (CSV/Excel exports)

# Seconds without a successful ESP32 /update before the dashboard calls the
# device disconnected. Tune with BRASSICA_DISCONNECT_TIMEOUT.
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
    # Internal bookkeeping (stripped before it reaches the browser).
    "_last_update_time": 0,
}


class StorageError(Exception):
    """Redis is configured but could not be reached / used right now."""


# ---------------------------------------------------------------------------
# Redis client (lazy; never touches the network at import time)
# ---------------------------------------------------------------------------

_redis_client = None
_redis_lock = threading.Lock()
_last_error = None          # (message, unix_time) of the most recent failure
_last_ok_time = 0.0


def _get_client():
    global _redis_client
    if _redis_client is not None:
        return _redis_client
    with _redis_lock:
        if _redis_client is None:
            try:
                import redis  # imported lazily so a missing package can't crash app import

                _redis_client = redis.from_url(
                    REDIS_URL,
                    decode_responses=True,
                    socket_connect_timeout=3,
                    socket_timeout=3,
                    retry_on_timeout=True,
                    health_check_interval=15,
                )
            except Exception as exc:  # noqa: BLE001
                raise StorageError(f"could not create Redis client: {exc}") from exc
        return _redis_client


def _reset_client():
    """Drop the cached client so the NEXT call builds a fresh connection."""
    global _redis_client
    with _redis_lock:
        _redis_client = None


def _run(fn):
    """Runs fn(client) against Redis, retrying once on a fresh connection.
    Raises StorageError if both attempts fail. Never returns stale data."""
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
        except Exception as exc:  # noqa: BLE001 - any redis/connection error
            last_exc = exc
        _reset_client()
    _last_error = (str(last_exc), time.time())
    print(f"[storage] Redis operation failed: {last_exc}")
    raise StorageError(str(last_exc))


# ---------------------------------------------------------------------------
# In-memory store (used only when REDIS_URL is NOT configured)
# ---------------------------------------------------------------------------

_mem_lock = threading.Lock()
_mem_latest = dict(DEFAULT_STATE)
_mem_history = deque(maxlen=MAX_HISTORY)
_mem_kv = {}                # name -> (json_string, expires_at or None)

# Last good copy of the live state served by THIS instance if Redis blips.
_cache_lock = threading.Lock()
_cache_latest = None


# ---------------------------------------------------------------------------
# Status helpers
# ---------------------------------------------------------------------------

def is_remote_configured():
    """True if a Redis connection string is set (regardless of reachability)."""
    return bool(REDIS_URL)


def backend_name():
    """'redis' when Redis is configured, else 'memory'."""
    return "redis" if REDIS_URL else "memory"


def last_error():
    """(message, unix_time) of the most recent Redis failure, or None."""
    return _last_error


def warning():
    """A human-readable problem with the storage setup, or None if it's fine.
    Shown as a banner on the dashboard so a bad setup is obvious."""
    if ON_VERCEL and not REDIS_URL:
        return ("Cloud storage is not set up. Add a Redis database in Vercel "
                "(Storage tab) and redeploy, otherwise live data will keep "
                "flickering on and off. See README.md, step 3.")
    if REDIS_URL and _last_error and (time.time() - _last_error[1]) < 60 \
            and _last_ok_time < _last_error[1]:
        return "The Redis database is not responding right now. Retrying..."
    return None


# ---------------------------------------------------------------------------
# Live state
# ---------------------------------------------------------------------------

def _decode_hash(raw):
    state = dict(DEFAULT_STATE)
    for field, value in (raw or {}).items():
        try:
            state[field] = json.loads(value)
        except (TypeError, ValueError):
            continue
    return state


def get_latest():
    """Returns the latest known state as a plain dict. If Redis is configured
    but momentarily unreachable, returns this instance's last good copy (its
    own timestamp makes it age out into 'disconnected' naturally)."""
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
    """Atomically merges `fields` into the live state (one Redis command)."""
    if REDIS_URL:
        mapping = {k: json.dumps(v) for k, v in fields.items()}
        _run(lambda c: c.hset(KEY_LATEST, mapping=mapping))
        return
    with _mem_lock:
        _mem_latest.update(fields)


def record_update(fields, history_row=None):
    """Used by POST /update: merges `fields` into the live state and, when
    `history_row` is given, appends it to the history list - all in a single
    Redis round-trip (one command for the state, two more for a history row)."""
    if REDIS_URL:
        mapping = {k: json.dumps(v) for k, v in fields.items()}

        def op(client):
            if history_row is None:
                client.hset(KEY_LATEST, mapping=mapping)
                return
            pipe = client.pipeline()
            pipe.hset(KEY_LATEST, mapping=mapping)
            pipe.lpush(KEY_HISTORY, json.dumps(history_row))
            pipe.ltrim(KEY_HISTORY, 0, MAX_HISTORY - 1)
            pipe.execute()

        _run(op)
        return
    with _mem_lock:
        _mem_latest.update(fields)
        if history_row is not None:
            _mem_history.appendleft(history_row)


def get_history():
    """Returns stored readings, oldest first, as a list of dicts."""
    if REDIS_URL:
        raw_rows = _run(lambda c: c.lrange(KEY_HISTORY, 0, MAX_HISTORY - 1))
        rows = []
        for raw in raw_rows:
            try:
                rows.append(json.loads(raw))
            except (TypeError, ValueError):
                continue
        rows.reverse()  # oldest first, like a CSV
        return rows
    with _mem_lock:
        return list(reversed(_mem_history))


def touch(now=None):
    """Returns (state, connected). `connected` is computed at READ time from
    `_last_update_time`, so no background thread is ever needed."""
    now = now if now is not None else time.time()
    state = get_latest()
    last_update = state.get("_last_update_time", 0) or 0
    connected = bool(last_update) and (now - last_update) <= DISCONNECT_TIMEOUT_SECONDS
    return state, connected


# ---------------------------------------------------------------------------
# Small JSON key/value store (Wi-Fi setup commands, scan results, ...)
# ---------------------------------------------------------------------------

def _kv_key(name):
    return PREFIX + name


def kv_set(name, value, ttl=None):
    """Stores any JSON-serialisable value; optional TTL in seconds."""
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
    """Returns {name: value-or-None} using a single Redis round-trip."""
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


# ---------------------------------------------------------------------------
# Auto-seed history from local CSV on cloud boot
# ---------------------------------------------------------------------------

def _seed_history_to_redis_if_empty():
    """Automatically seeds sensor_data.csv into Redis if the cloud history is empty."""
    if not REDIS_URL:
        return
    try:
        client = _get_client()
        count = client.llen(KEY_HISTORY)
        if count > 0:
            return  # Already populated

        csv_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sensor_data.csv")
        if not os.path.exists(csv_path):
            return

        df = pd.read_csv(csv_path)
        history_list = []
        for _, row in df.iterrows():
            history_list.append({
                "timestamp": str(row["Timestamp"]),
                "temp": float(row["Temperature_C"]) if str(row["Temperature_C"]) != "ERR" else "ERR",
                "hum": float(row["Humidity_pct"]) if str(row["Humidity_pct"]) != "ERR" else "ERR",
                "soil1": float(row["Soil_1_pct"]) if str(row["Soil_1_pct"]) != "ERR" else "ERR",
                "soil2": float(row["Soil_2_pct"]) if str(row["Soil_2_pct"]) != "ERR" else "ERR",
                "pump": str(row["Pump_Active"]).lower() in ("true", "1", "on"),
                "fan": str(row["Exhaust_Fan_Active"]).lower() in ("true", "1", "on")
            })

        if history_list:
            json_rows = [json.dumps(row) for row in history_list]
            client.rpush(KEY_HISTORY, *json_rows)
            print(f"[storage] Successfully auto-seeded {len(history_list)} historical records into cloud Redis.")
    except Exception as exc:
        print(f"[storage] Failed to auto-seed history: {exc}")

if ON_VERCEL and REDIS_URL:
    threading.Thread(target=_seed_history_to_redis_if_empty, daemon=True).start()