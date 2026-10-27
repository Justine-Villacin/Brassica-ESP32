# Changelog

## v6.0 — Brassica (renamed from the previous project name) · connection fixes · Wi-Fi from the website

### Problems reported, and their root causes

1. **Hosted version flickers connected / disconnected.**
   * No shared storage guarantee: the old `storage.py` fell back to a private
     in-memory dict if Redis answered slowly **once**, and remembered that failure
     for the life of the serverless instance — different instances then showed
     different data. Now a configured Redis is never silently replaced; failures
     are retried, reads fall back to the last good copy, and the ESP32 gets an
     HTTP 503 it simply retries.
   * Disconnect timeout was 8 s while a hosted post (cold start + TLS) can take
     longer → now 35 s, plus two-answer confirmation in the browser.
   * The ESP32 ran a blocking 2–5 s Wi-Fi scan and the scan could drop the link →
     scan is now asynchronous and posting waits while it runs.
   * `pandas` was in `requirements.txt` (never used) → slow cold starts → removed.
   * Updates were read-modify-write of one JSON blob, so the ESP32, the
     dashboard and control calls could overwrite each other → state is now a
     Redis hash updated with one atomic command.
   * Dashboard polling every 2 s even from hidden tabs would exhaust Vercel's and
     Redis's free quotas → 5 s active / 30 s idle / paused when hidden; ESP32
     posts every 10 s hosted; history every 5 min (≈ 1 Redis command per post).
   * No `Cache-Control` on live endpoints → now `no-store`.
2. **`desktop.py`: ESP32 never connects, same IP and Wi-Fi.** The v5.2 firmware
   built **invalid JSON** (it closed the object, then appended the Wi-Fi list
   after it), so the server answered HTTP 400 to every post. Fixed (one valid
   object; verified by compiling the sketch against mock APIs and parsing the
   output). The server is also tolerant of odd bytes in network names, logs
   rejected bodies, `desktop.py` prints the PC's LAN address and the exact
   ESP32 settings, reports `[ESP32] Connected` / `REJECTED with HTTP …`, checks
   that port 8000 is free, and falls back to the browser if the window library
   is missing.
3. **Wi-Fi chosen from the website** — new, see README §3: ESP32 scan list,
   trial-with-rollback, `Brassica-Setup-XXXX` fallback portal, BOOT-button
   shortcuts, settings saved in flash. The dashboard dialog was a stub before.
4. **Renamed everything to Brassica** (titles, manifest, service-worker cache,
   download file names, environment variables `BRASSICA_*`, Redis key prefix,
   firmware folder/sketch names, docs). Environment variables from the previous
   name are no longer read — if you had set an API key on Vercel, set
   `BRASSICA_API_KEY` (and keep `API_KEY` in `config.h` identical).

### Other bugs fixed along the way

* Spare relay and warm-light relay were both on GPIO 33 → the light was forced
  OFF every cycle. Spare moved to GPIO 32; diagnostics sketch and docs aligned
  with `config.h` (DHT 26, light relay 33).
* A network name containing `'` broke the Wi-Fi list, and a hostile name could
  inject HTML. Names are now inserted as plain text.
* `main.js` wrote to a non-existent footer element, throwing on every poll;
  export errors were invisible. Both fixed (footer line + toasts).
* `/api/history` crashed on "Feb 29" (year-less date parsing) and ordered days
  by month name; now uses real dates and a configurable time zone (default
  `Asia/Manila`, since Vercel runs in UTC).
* `/api/toggle` (a GET that changed state, used by nothing) removed.
* Service worker no longer touches POSTs or other sites; stray closing tags in
  `index.html`, a 0-byte `bg.png`, a mislabeled 192 px icon, and stale
  threshold texts were fixed.
* Removed the 1 MB of "ERR" log rows, `.venv`, `.git` and `__pycache__` from the package.
