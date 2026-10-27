# Brassica User Manual

A plain-language guide. You do not need to read `README.md` first.

## Part 0 — Put the firmware on the ESP32 (once)

1. Install the **Arduino IDE** (arduino.cc/en/software). Open **Boards Manager**,
   install **esp32** by Espressif. Open **Tools → Manage Libraries** and install
   **DHT sensor library** by Adafruit.
2. Open `firmware/Brassica_Diagnostics/Brassica_Diagnostics.ino`, choose board
   **ESP32 Dev Module** and your port, upload. Open the Serial Monitor (115200).
   Confirm the sensors give numbers and each relay clicks. Write down the soil
   and light numbers (dry/wet, dark/bright).
3. Open `firmware/Brassica_ESP32/Brassica_ESP32.ino`. In the `config.h` tab fill
   in your calibration numbers, your Wi-Fi name/password, and the server (Part 1
   or Part 2 below). Upload.

## Part 1 — On your PC (no internet needed)

1. Install Python 3.10+ from python.org (tick *Add to PATH*).
2. In the project folder, in a terminal:
   ```
   python -m venv .venv
   .venv\Scripts\activate
   pip install -r requirements-desktop.txt
   python desktop.py
   ```
3. The window opens. The terminal shows **your PC's address** (for example
   `192.168.1.53`). Put that in `config.h` as `SERVER_HOST`, keep
   `SERVER_USE_HTTPS false` and `SERVER_PORT 8000`, and upload again.
4. In a moment the terminal prints `[ESP32] Connected` and the badge on the
   dashboard turns green.

Not connecting? PC and ESP32 must be on the same Wi-Fi; allow Python through
Windows Firewall (Private networks) when asked; and re-check the address if
your router restarted. README §2 has the full checklist.

## Part 2 — On the internet (Vercel)

1. Upload the project to GitHub and import it at vercel.com/new.
2. In your Vercel project open **Storage → Create Database → Upstash (Redis)**,
   choose the free plan, **Connect to Project**, then **Redeploy**. *(Do not skip
   this: it is what stops the dashboard flickering on and off.)*
3. Open `https://your-site.vercel.app/api/status` — it must say
   `"storage_backend": "redis"`.
4. In `config.h` set `SERVER_USE_HTTPS true`, `SERVER_HOST "your-site.vercel.app"`
   (no `https://`, no `/`), `SERVER_PORT 443`; upload to the ESP32.
5. Recommended: in Vercel → Settings → Environment Variables add
   `BRASSICA_ADMIN_PIN` (a PIN of your choice) and redeploy.

## Part 3 — Changing the ESP32's Wi-Fi without touching the code

1. On the dashboard click the **Wi-Fi icon** (top right).
2. Choose a network from the list (the ESP32 found them), type the password,
   press **Connect** (type your PIN if asked).
3. Wait up to a minute. The window tells you if the ESP32 joined. If it could
   not, it automatically returned to its old network and tells you why
   (usually a wrong password).

**If the ESP32 cannot get online at all:** after about a minute it creates its
own Wi-Fi, **Brassica-Setup-XXXX** (password `brassica123`). Connect your phone
to it, open `http://192.168.4.1`, choose your Wi-Fi and password, Save. You can
also force this by holding the **BOOT** button for 3 seconds (hold 10 seconds to
erase everything it remembers).

## Part 4 — Using the dashboard

* **Sensors / Auto / Export** tabs (bottom bar on phones).
* *Automation* cards show Active, Standby, or Offline (its sensor is
  disconnected, so the device is forced off for safety).
* *Recent* and the arrow button show daily averages for the last 7 days.
* **Download full Excel report** exports the history (saved every 5 minutes on
  Vercel; every 2 s into `sensor_data.csv` on your PC).
* **Install** adds Brassica to your phone's home screen.

## Quick fixes

| Symptom | Try |
|---|---|
| Badge flickers on Vercel | Part 2 step 2–3 (Redis). Red banner on the page = Redis missing. |
| "ESP32 DISCONNECTED" on PC mode | Part 1 "Not connecting?" |
| ESP32 keeps restarting | Power: 5 V / 2 A supply; diodes across pump and fan. |
| Wi-Fi dialog says "ESP32 is offline" | The request waits (10 min) and applies when it returns; or use the Brassica-Setup Wi-Fi. |
| Sensor shows OFFLINE | Check that sensor's wiring; its device is switched off on purpose. |
