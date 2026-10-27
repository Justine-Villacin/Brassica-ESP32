# Brassica — ESP32 Greenhouse Monitor (v6.0)

An automated greenhouse dashboard for an ESP32-based Brassica grow setup: live
temperature / humidity / soil / light readings, pump / grow-light / fan
automation status, a plant-health score, 7-day averages, CSV/Excel export, and
**changing the ESP32's Wi-Fi from the website — no re-flashing.**

| Mode | Command / host | Use for |
|---|---|---|
| **Desktop app** | `python desktop.py` on your PC | Day-to-day monitoring on your home Wi-Fi |
| **Hosted web app** | Vercel (`app.py`) | A public link you can open from anywhere |

Both run the exact same Flask routes, templates and static files. The ESP32
firmware (`firmware/`) works with either; the automation (pump / light / fan)
runs **on the ESP32 itself**, so it keeps working if Wi-Fi or the website is down.

---

## 1. Deploy on Vercel (read step 3 — it is the cure for the on/off flicker)

1. **Put the project on GitHub** (unzip it, then upload/push the folder), or use
   the Vercel CLI: `npm i -g vercel`, then `vercel --prod` inside the folder.
2. **Import it** at [vercel.com/new](https://vercel.com/new). No build settings
   are needed — Vercel detects the Flask `app` in `app.py` by itself.
3. **Add a Redis database. This step is mandatory.**
   Open your project → **Storage** → **Create Database** → choose **Upstash
   (Redis)** → pick the free plan and a region close to you (Singapore if you are
   in the Philippines) → **Connect to Project**. Vercel adds the connection
   string (`REDIS_URL` / `KV_URL`) for you.
   *Why:* Vercel runs the site as serverless functions; each request can land on
   a fresh instance that remembers nothing. Without a shared database the ESP32's
   data and the dashboard never see each other reliably, which is exactly the
   "connected… disconnected… connected" behaviour.
4. **Redeploy** (Deployments → ⋯ → *Redeploy*). Environment variables only
   reach new deployments.
5. **Check it:** open `https://<your-site>.vercel.app/api/status`. You must see
   `"storage_backend": "redis"` and `"storage_warning": null`. If it says
   `memory`, step 3 or 4 was missed (the dashboard also shows a red banner).
6. **Optional but recommended** (Project → Settings → Environment Variables,
   then redeploy):

   | Variable | Purpose |
   |---|---|
   | `BRASSICA_ADMIN_PIN` | PIN required on the dashboard before the ESP32's Wi-Fi can be changed. Without it, anyone who knows your URL can. |
   | `BRASSICA_API_KEY` | Secret the ESP32 must send with every update. Put the same value in `API_KEY` in `firmware/Brassica_ESP32/config.h`. |
   | `BRASSICA_TZ` | Time zone for history/exports (default `Asia/Manila`; Vercel itself runs in UTC). |
   | `BRASSICA_DISCONNECT_TIMEOUT` | Seconds of silence before "ESP32 DISCONNECTED" (default 35). |

   *Tip:* Settings → Functions → **Function Region** → Singapore shortens each
   round trip for a device in the Philippines.
7. **Point the ESP32 at it** — see [firmware/README.md](firmware/README.md): set
   `SERVER_USE_HTTPS true` and `SERVER_HOST "<your-site>.vercel.app"` (bare
   domain: no `https://`, no trailing `/`), flash once.

### What keeps it from flickering (v6.0)

* The dashboard waits **35 s** of silence (was 8 s) and needs two "offline"
  answers in a row before it shows the offline view.
* The ESP32 posts every 10 s when hosted; a cold start or slow TLS handshake can
  no longer look like a disconnect.
* If Redis has a hiccup, the ESP32 simply gets an error and retries, and the
  dashboard keeps showing its last good data — no more instances quietly
  keeping private copies of the data.
* The scan of nearby Wi-Fi networks no longer blocks the ESP32 for seconds.
* `pandas` (unused, ~100 MB) was removed, so cold starts are fast.
* Live-data responses are `Cache-Control: no-store`.

### Free-tier budget (why the intervals are what they are)

Upstash's free plan allows **500,000 Redis commands per month**. Brassica is
built to fit:

| Source | Commands |
|---|---|
| ESP32 post, every 10 s | 1 each (~260k / month) |
| ESP32 "any Wi-Fi change for me?" check, every 30 s | 1 each (~86k / month) |
| History row, every 5 min | 3 each (~26k / month) |
| Dashboard refresh (5 s while you use it, 30 s when idle, none when hidden) | 1 each |

That is roughly 370k/month plus your own viewing. If you ever pass the quota,
the database starts refusing requests and the dashboard flickers again; the
fix is Upstash pay-as-you-go ($0.20 per 100,000 extra commands, usually well
under $1/month). `/api/status` shows `last_storage_error` if that happens.

---

## 2. Desktop mode (talks to the real ESP32 on your Wi-Fi)

```bash
python -m venv .venv
.venv\Scripts\activate           # macOS/Linux: source .venv/bin/activate
pip install -r requirements-desktop.txt
python desktop.py                # python desktop.py --no-window  = use your browser
```

On start it prints **your PC's address on the network** and exactly what the
ESP32 must be set to (`SERVER_HOST`, `SERVER_PORT 8000`, `SERVER_USE_HTTPS
false`). It also prints a line like `[ESP32] Connected: receiving data from
192.168.1.40` the moment the ESP32 arrives, or `REJECTED with HTTP …` if it is
refused — so you always know what is happening.

**If the ESP32 does not connect** (check in this order):

1. **Same Wi-Fi?** PC and ESP32 must be on the same router (2.4 GHz for the
   ESP32). A guest network or a "client isolation" setting blocks it.
2. **Right address?** `SERVER_HOST` must equal the address `desktop.py` printed.
   Routers can hand the PC a new address after a restart — re-check it, or
   reserve a fixed address for the PC in the router settings. You can also type
   the new address into the ESP32's `Brassica-Setup` page (firmware README).
3. **Windows Firewall** (the most common cause). The first time Python listens,
   Windows asks for permission; if you pressed *Cancel*, the ESP32 is blocked.
   Allow Python on **Private** networks, or run once in an *Administrator*
   Command Prompt:
   `netsh advfirewall firewall add rule name="Brassica" dir=in action=allow protocol=TCP localport=8000`
4. **Port in use?** `desktop.py` now refuses to start and tells you if port 8000
   is already taken by an old copy.
5. Open `http://<PC address>:8000/api/status` from your phone (same Wi-Fi). If
   that does not load, it is the network/firewall, not the ESP32.
6. Watch the ESP32's Serial Monitor (115200): it prints the HTTP result of each post.

Desktop mode logs to `sensor_data.csv` next to `desktop.py` and uses plain
in-memory state (no Redis needed). Live reload is on by default; set
`BRASSICA_DEV_MODE=false` to turn it off. Put `cert.pem` / `key.pem` next to
`desktop.py` for an extra HTTPS server on port 8443 (phone "install app" prompt).

---

## 3. Changing the ESP32's Wi-Fi from the website

Click the **Wi-Fi icon** (top right):

1. The list shows networks the **ESP32** can see (it scans for you), the one it
   is on, and its IP and signal.
2. Pick one (or **Other network** for a hidden one), type the password (and the
   dashboard PIN if you set `BRASSICA_ADMIN_PIN`), press **Connect**.
3. Within about 30 seconds the ESP32 picks the request up, tries the new
   network, and the dialog reports the result.

**It cannot get you stuck:** the old network is kept until the new one is
confirmed working *and* the ESP32 has reached the dashboard over it. If the
password is wrong, the network is out of range, or the dashboard is unreachable
there, the ESP32 goes back by itself within 45 s and the dialog tells you why.

If the ESP32 cannot reach any network, it opens its own Wi-Fi called
**`Brassica-Setup-XXXX`** (password `brassica123`, change it in `config.h`).
Join it with a phone, open `http://192.168.4.1` (most phones open it
automatically), type the network. You can also hold the **BOOT** button for 3 s
to open it on purpose, or 10 s to erase every saved setting.

How it works: the website stores a short-lived request (expires after 10 min)
→ the ESP32 receives it in the reply to its next check-in → applies it → reports
back. Credentials travel inside your own deployment only; the password is
deleted as soon as the ESP32 confirms. Over `desktop.py` (plain HTTP on your
LAN) the same applies on your home network.

---

## `POST /update` — what the ESP32 sends

| Field | Notes |
|---|---|
| `temperature`, `humidity`, `soil_moisture`, `light` | Numbers; omitted when that sensor is down. `light` is the LDR reading 0-1000. |
| `dht_connected`, `soil_connected`, `light_connected` | Booleans. |
| `fan`, `water_pump`, `grow_light` | `"ON"`/`"OFF"`. `grow_light` is the warm-light **relay**, different from `light`. |
| `wifi_ssid`, `ip`, `rssi`, `fw` | Device info shown in the Wi-Fi dialog. |
| `ctl` | `true` ≈ every 30 s: "any Wi-Fi change / rescan waiting?" |
| `log` | `true` ≈ every 5 min: "save a history row". |
| `wifi_networks` | `[{"ssid","rssi","secure"}]`, sent after a scan. |
| `wifi_ack` | `{"id","status":"ok"\|"failed","ssid","reason"}`, result of a Wi-Fi change. |

Reply: `{"status":"success"}`, plus optionally `"scan":true` and
`"wifi_config":{"id","ssid_hex","pass_hex"}` (hex-encoded so no character can
break the JSON). The server also forces a device OFF in its report if the
sensor it depends on is disconnected.

Other endpoints: `/api/sensors`, `/api/status`, `/api/history`,
`/sensor_data.csv`, `/download/excel`, `/api/wifi/{scan,rescan,status,configure}`.

## Project layout

```
app.py                 Flask app — the only file Vercel runs
storage.py             Redis-backed (or in-memory) state store
desktop.py             Desktop window, local CSV log, live reload (PC only)
templates/index.html   Dashboard page
public/                CSS, JS (main.js, wifi.js, ui.js…), icons, manifest, sw.js
firmware/              ESP32 sketches: Brassica_ESP32 (real) + Brassica_Diagnostics
requirements.txt       What Vercel installs
requirements-desktop.txt   Adds pywebview for desktop.py
vercel.json, .vercelignore, .gitignore, .env.example
```

## Updating later

Changing only thresholds / pins / calibration in `config.h` → just re-flash; the
website is untouched. Changing the website → redeploy; the ESP32 needs nothing.

## License

Apache License 2.0 — see `LICENSE`.
