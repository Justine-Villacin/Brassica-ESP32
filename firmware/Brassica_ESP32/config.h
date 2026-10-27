// config.h — Brassica ESP32 firmware configuration (v6.2).
// Edit the values below, then flash Brassica_ESP32.ino.
// (In the Arduino IDE this appears as a second tab next to the .ino file —
// you don't need to "include" it yourself, just fill it in.)
//
// TIP: after the first flash you normally never have to touch this file again.
//   * Change the Wi-Fi network from the dashboard (the Wi-Fi icon, top right).
//   * If the ESP32 cannot connect at all, it opens its own Wi-Fi called
//     "Brassica-Setup-XXXX" — connect to it and enter the network there.
// The values below are only the DEFAULTS used when nothing was saved yet.

#ifndef BRASSICA_CONFIG_H
#define BRASSICA_CONFIG_H

#define FIRMWARE_VERSION "6.2"

// ============================================================
// Default Wi-Fi  (ESP32 supports 2.4 GHz networks ONLY — not 5 GHz)
// ============================================================
// Leave both as "" if you want to set Wi-Fi only from the dashboard / the
// Brassica-Setup page.
#define WIFI_SSID        "Delinia Fam 2.4G"
#define WIFI_PASSWORD    "03121980"

// ============================================================
// Default server — pick ONE target
// ============================================================
// --- Local desktop mode (python desktop.py on your PC) — ACTIVE ---
//     SERVER_HOST = the "address on this network" that desktop.py prints
//     when it starts (it can change if your router hands out a new IP).
#define SERVER_USE_HTTPS   true
#define SERVER_HOST        "brassicarapa.vercel.app"
#define SERVER_PORT        443 // unused while SERVER_USE_HTTPS is true, but must stay defined
//
// --- Hosted on Vercel --- (comment the 3 lines above, uncomment these)
// SERVER_HOST is the BARE DOMAIN ONLY — the firmware builds the full URL.
//   RIGHT: "brassica.vercel.app"
//   WRONG: "https://brassica.vercel.app/"
// #define SERVER_USE_HTTPS   true
// #define SERVER_HOST        "brassica.vercel.app"
// #define SERVER_PORT        443
#define UPDATE_PATH        "/update"

// Must exactly match BRASSICA_API_KEY on the server. Leave "" while
// BRASSICA_API_KEY is unset server-side.
#define API_KEY             ""

// ============================================================
// Pins (ESP32 DevKit v1 / similar 30-38 pin boards)
// ============================================================
// SENSORS
#define DHT_PIN               26     // DHT11 data pin
#define DHT_TYPE              DHT11
#define SOIL1_PIN             32     // soil sensor 1 analog out (ADC1)
#define SOIL2_PIN             35     // soil sensor 2 analog out (ADC1, input-only pin)
// NOTE: only use ADC1 pins (32-39) for the soil sensors. ADC2 pins do not
// work while Wi-Fi is on.

// 4-CHANNEL RELAY MODULE (IN1..IN4): every relay needs its OWN pin
#define RELAY_PUMP_PIN        25     // IN1 -> 12V water pump
#define RELAY_FAN2_PIN        33     // IN2 -> 12V exhaust fan 2
#define RELAY_FAN1_PIN        27     // IN3 -> 12V exhaust fan 1
#define RELAY_SPARE_PIN       14     // IN4 -> spare channel (kept OFF)

#define STATUS_LED_PIN         2     // onboard LED on most ESP32 DevKit boards
#define BOOT_BUTTON_PIN        0     // the "BOOT" button on the board

// Most 4-channel relay modules switch ON when the IN pin is driven LOW
// ("active-low"). If yours turns ON when driven HIGH, set this to false.
#define RELAY_ACTIVE_LOW     true

// ============================================================
// Soil calibration — REQUIRED before trusting the readings
// ============================================================
// HOW TO CALIBRATE (each sensor separately, pump OFF):
//   1. Flash this firmware and open the Serial Monitor (115200). Every 2.5 s it
//      prints "Soil 1: xx%  (raw NNNN)" for each sensor.
//   2. Hold the sensor in DRY AIR, wait ~20 s until raw is steady
//      -> write that number as *_RAW_DRY  (this will be 0 %).
//   3. Put the sensor into WATER up to the line marked on the sensor
//      (do not submerge the electronics), wait ~20 s
//      -> write that number as *_RAW_WET  (this will be 100 %).
//   4. Re-flash. Dry air must show ~0 %, water ~100 %.
// The numbers below are only placeholders. Your sensors will be different,
// and two sensors of the same model usually differ from each other too.
#define SOIL1_RAW_DRY         3300
#define SOIL1_RAW_WET         2000
#define SOIL2_RAW_DRY         3000
#define SOIL2_RAW_WET         480

// ============================================================
// Soil reading quality
// ============================================================
#define ADC_SAMPLES                 32   // analog reads per measurement. The lowest and highest
                                         // quarter are thrown away, the middle half is averaged
                                         // (rejects Wi-Fi / relay spikes)
#define SOIL_SMOOTH_ALPHA         0.35f  // 0.05 = very smooth/slow ... 1.0 = no smoothing
#define SOIL_NOISE_LIMIT           250   // spread (counts) of the middle half of the samples that
                                         // marks a sensor as floating/disconnected. A healthy
                                         // sensor is usually < 60. Tune with Serial Monitor.
#define SOIL_RAIL_LOW               20   // raw at/below this = shorted to GND = fault
#define SOIL_RAIL_HIGH            4075   // raw at/above this = shorted to 3V3 / unplugged = fault
#define RAIL_COUNT_THRESHOLD         2   // bad measurements IN A ROW before a soil sensor is
                                         // declared "disconnected" (a single glitch is ignored
                                         // and the last good value is kept)

// While the pump is running (automatic OR manual) the soil readings are
// FROZEN at the last clean value taken before the pump started. Running the
// 12 V pump and wet soil disturb the sensors and make the numbers jump.
// The reading stays frozen for SOIL_SETTLE_MS after the pump stops, then live
// readings (smoothed) resume.   1 = freeze, 0 = always read live
#define SOIL_HOLD_WHILE_PUMP         1
#define SOIL_SETTLE_MS           10000   // 10 s

// ============================================================
// Automation thresholds
// ============================================================
// Exhaust fans (temperature). Both default to the same values so they run
// together. To run them in stages, give fan 2 a HIGHER temperature than fan 1.
#define TEMP_FAN1_ON_C         30.0   // fan 1 ON at/above this
#define TEMP_FAN1_OFF_C        29.9   // fan 1 OFF at/below this
#define TEMP_FAN2_ON_C         30.0   // fan 2 ON at/above this
#define TEMP_FAN2_OFF_C        29.9   // fan 2 OFF at/below this

// Pump (soil moisture) — AUTOMATIC mode only.
#define SOIL_PUMP_ON_PCT        60    // pump ON at/below this
#define SOIL_PUMP_OFF_PCT       80    // pump OFF at/above this
// NOTE: because the soil reading is frozen while the pump runs, the automatic
// pump works in PULSES: ON for up to PUMP_MAX_RUN_MS, then rest for
// PUMP_COOLDOWN_MS while the water soaks in, then it measures again and
// repeats until the soil is above SOIL_PUMP_ON_PCT. It stops early only when
// SOIL_HOLD_WHILE_PUMP is 0.

// Which soil value drives the pump when both sensors work:
//   0 = the AVERAGE of the two sensors
//   1 = the DRIEST sensor (waters until even the dry spot is wet)
// If only one sensor is working, that one is used. If both fail, the
// AUTOMATIC pump is forced OFF.
#define SOIL_CONTROL_MODE 1     // 0 = average, 1 = driest

// ============================================================
// Manual override (dashboard "Water Pump Override" button)
// ============================================================
// While override is active the pump is driven ONLY by the dashboard:
//   - it runs until the dashboard turns the override OFF,
//   - automatic soil logic, max-run and cooldown are ignored,
//   - it keeps running even if the soil sensors are down.
// Every server reply must keep confirming the override. If the ESP32 reboots,
// or no confirmation arrives for MANUAL_OVERRIDE_TIMEOUT_MS (Wi-Fi/server
// lost), the override is dropped and the pump goes OFF, so a stale command can
// never leave the pump running forever.
#define MANUAL_OVERRIDE_TIMEOUT_MS   120000UL   // 2 min

// Send two extra fields to the server (pump_override, soil_hold) so the
// dashboard can show "MANUAL" and "reading held". Set to 0 if your server
// rejects unknown fields.
#define REPORT_EXTRA_FIELDS          1

// ============================================================
// Relay protection
// ============================================================
// After any relay changes state it will not be switched again by a
// threshold for this long. (Fail-safe OFF and manual override are immediate.)
#define MIN_RELAY_HOLD_MS     30000

// Pump safety (AUTOMATIC mode): a pump never runs longer than PUMP_MAX_RUN_MS
// in one go, then it rests for PUMP_COOLDOWN_MS so water can soak in.
#define PUMP_MAX_RUN_MS       30000   // 30 s
#define PUMP_COOLDOWN_MS      60000   // 1 min

// ============================================================
// Timing (milliseconds)
// ============================================================
#define SENSOR_READ_INTERVAL_MS     2500   // DHT11 needs >= 2 s between reads

// How often a reading is sent to the dashboard.
//   Hosted (HTTPS, e.g. Vercel): every 10 s (keeps the free Redis plan under quota).
//   Local (plain HTTP to desktop.py): every 3 s.
#define SERVER_POST_INTERVAL_HOSTED_MS   5000
#define SERVER_POST_INTERVAL_LOCAL_MS     2000

// How often the ESP32 asks the server for pending commands (Wi-Fi change /
// rescan). While the manual pump override is active it asks on EVERY post.
#define CTL_CHECK_INTERVAL_HOSTED_MS     10000   // 10 s
#define CTL_CHECK_INTERVAL_LOCAL_MS       3000   // 3 s

// How often a row is added to the history (CSV export / 7-day averages).
#define HISTORY_LOG_INTERVAL_MS         300000
#define HTTP_CONNECT_TIMEOUT_MS     6000
#define HTTP_RESPONSE_TIMEOUT_MS    8000
#define MIN_HEAP_FOR_TLS           40000

// ============================================================
// Wi-Fi behaviour
// ============================================================
#define WIFI_ATTEMPT_MS            15000
#define WIFI_REBOOT_AFTER_MS      600000   // reboot if Wi-Fi has been down this long (0 = never)
#define WIFI_TRIAL_TIMEOUT_MS      45000
#define WIFI_SCAN_INTERVAL_MS     300000
#define MAX_SCAN_NETWORKS             10

// ============================================================
// Setup portal ("Brassica-Setup-XXXX" Wi-Fi)
// ============================================================
#define PORTAL_AP_PREFIX            "Brassica-Setup"
#define PORTAL_AP_PASSWORD          "brassica123"   // 8+ characters, or "" for an open network
#define PORTAL_AFTER_WIFI_DOWN_MS  60000
#define PORTAL_AFTER_POST_FAILS       24

// ============================================================
// DHT fault detection
// ============================================================
#define DHT_FAIL_THRESHOLD           2   // consecutive DHT failures before "disconnected"

#endif // BRASSICA_CONFIG_H