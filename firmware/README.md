# Brassica Firmware (v6.0)

| Sketch | Purpose |
|---|---|
| `Brassica_Diagnostics/Brassica_Diagnostics.ino` | Run **first**. No Wi-Fi. Tests every sensor and relay and gives the raw numbers for calibration. |
| `Brassica_ESP32/Brassica_ESP32.ino` + `config.h` | The real firmware: sensors, local automation with fail-safes, Wi-Fi, reporting to the dashboard, Wi-Fi setup without re-flashing. |

## Hardware

ESP32 DevKit (30/38 pin), DHT11, soil moisture sensor (analog), light sensor
(LDR module, analog), 4-channel relay module, 12 V fan + pump, warm light, a
12 V supply and a **5 V buck converter** for the ESP32 and relay board.

## Wiring (matches `config.h`)

| Component | ESP32 pin |
|---|---|
| DHT11 data (3.3 V / GND; 10 k pull-up if it is a bare 4-pin sensor) | **GPIO 26** |
| Soil sensor analog out | **GPIO 34** |
| Light sensor analog out | **GPIO 35** |
| Relay `IN1` → 12 V pump | **GPIO 25** |
| Relay `IN2` → warm light | **GPIO 33** |
| Relay `IN3` → 12 V fan | **GPIO 27** |
| Relay `IN4` → spare (kept OFF) | **GPIO 32** |

Relay `VCC` to 5 V (buck converter), and **join all grounds** (ESP32, relay,
buck converter, 12 V supply). Use the relay's **NO** contact so devices are OFF
when unpowered. Put a **1N4007 diode across the pump and the fan** (stripe
toward +): motor spikes are a classic cause of ESP32 resets and Wi-Fi dropouts.
A **mains** warm light must be wired by a qualified person with a relay rated
for it.

> If you wired the DHT on a different pin than 26, or the light relay on a
> different pin than 33, change `DHT_PIN` / `RELAY_LIGHT_PIN` in `config.h`
> (and in the diagnostics sketch). Every relay needs its own pin — v5.2 had the
> spare and the light on the same pin, which kept the light OFF.

## 1. Install

* Arduino IDE → **Boards Manager** → install **esp32** by Espressif; board
  **ESP32 Dev Module**.
* **Tools → Manage Libraries** → **DHT sensor library** by Adafruit (accept
  *Adafruit Unified Sensor*). Nothing else is needed (ArduinoJson is not used).

## 2. Diagnostics first

Upload `Brassica_Diagnostics`, Serial Monitor **115200**, line ending
*Newline*. Check the DHT prints numbers; note soil raw values (dry air / in
water) and light raw values (dark / bright). Test relays with **nothing
connected to their outputs**: `p1 p0 l1 l0 f1 f0 s1 s0 a0`. If `1` switches a
relay OFF, set `RELAY_ACTIVE_LOW false` in both sketches.

## 3. Configure and flash `Brassica_ESP32`

Open `config.h` (second tab) and set:

1. Calibration: `SOIL_RAW_*`, `LDR_RAW_*`.
2. Default Wi-Fi: `WIFI_SSID`, `WIFI_PASSWORD` (2.4 GHz). You may leave them `""` and
   set Wi-Fi from the website / setup page instead.
3. Server — pick ONE block:
   * **Local PC** (`python desktop.py`): `SERVER_USE_HTTPS false`, `SERVER_HOST` =
     the address `desktop.py` printed, `SERVER_PORT 8000`.
   * **Vercel**: `SERVER_USE_HTTPS true`, `SERVER_HOST "your-site.vercel.app"`
     (bare domain only), `SERVER_PORT 443`.
4. `API_KEY` only if you set `BRASSICA_API_KEY` on the server (same value).

Upload, open Serial Monitor at 115200. A healthy start looks like:

```
[Brassica] Booting v6.0...
[WiFi] Connected. IP 192.168.1.40  RSSI -52 dBm
[Brassica] POST OK (200)
```

## Changing Wi-Fi / server later without re-flashing

* **From the dashboard:** Wi-Fi icon → pick network → password → Connect. The
  ESP32 tests it (45 s), keeps the old network if it fails, and reports why.
* **Setup Wi-Fi:** if no saved network works for 60 s (or the server is
  unreachable for ~4 min hosted / ~1 min local), the ESP32 opens **`Brassica-Setup-XXXX`**
  (password `brassica123`). Join, open `http://192.168.4.1`, choose the network;
  the *advanced* section also lets you change the server address, port and
  HTTPS. Saved to flash; the ESP32 restarts and joins.
* **BOOT button:** hold 3 s = open the setup Wi-Fi now. Hold 10 s = erase all
  saved settings (back to the `config.h` defaults).
* Saved network is tried first, then the `config.h` default, so a bad saved
  network can never strand the device.

## Automation behaviour (from `config.h`)

| Device | ON | OFF | Safety |
|---|---|---|---|
| Fan | temp ≥ 30 °C | temp ≤ 29.9 °C | OFF if the DHT fails 3 reads in a row |
| Pump | soil ≤ 40 % | soil ≥ 55 % | OFF if the soil sensor fails; max 30 s per run, then 2 min rest |
| Warm light | light < 300 | light ≥ 450 | OFF if the light sensor faults |

Each relay waits ≥ 15 s between threshold switches (fail-safe OFF is instant).
Automation runs on the ESP32 itself and continues without Wi-Fi.

## Reading the Serial Monitor

| Message | Meaning |
|---|---|
| `POST OK (200)` | Everything works. |
| `POST -> HTTP 400` | The server rejected the data (read the text after it). |
| `401 …API_KEY…` | `API_KEY` in `config.h` ≠ `BRASSICA_API_KEY` on the server. |
| `503 … Redis` | Hosted dashboard has no (or an unreachable) Redis database — README §1 step 3. |
| `POST failed: connection refused` | Wrong `SERVER_HOST`/port, server not running, or PC firewall (local mode). |
| `Disconnected, reason code 201 / 202 / 15` | Network not found (is it 2.4 GHz?) / wrong password. |
| `Brownout detector was triggered` | Power problem: use a proper 5 V/2 A supply, add the motor diodes. |

Signal worse than about −75 dBm is unreliable; move the router or ESP32 closer.
