// config.h — Brassica ESP32 firmware configuration (v6.1).
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

#define FIRMWARE_VERSION "6.0"

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
#define SERVER_USE_HTTPS   false
#define SERVER_HOST        "192.168.100.217"
#define SERVER_PORT        8000   // unused while SERVER_USE_HTTPS is true, but must stay defined
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
// Hardware: 1 x DHT11, 2 x soil moisture sensors, 2 x exhaust fans, 1 x pump,
// 4-channel relay module (12 V loads switched through the relays).
//
// SENSORS
#define DHT_PIN               26     // DHT11 data pin
#define DHT_TYPE              DHT11
#define SOIL1_PIN             32     // soil sensor 1 analog out (ADC1, input-only pin)
#define SOIL2_PIN             35     // soil sensor 2 analog out (ADC1, input-only pin)

// 4-CHANNEL RELAY MODULE (IN1..IN4): every relay needs its OWN pin
#define RELAY_PUMP_PIN        25     // IN1 -> 12V water pump
#define RELAY_FAN2_PIN        33     // IN2 -> 12V exhaust fan 2
#define RELAY_FAN1_PIN        27     // IN3 -> 12V exhaust fan 1
#define RELAY_SPARE_PIN       14     // IN4 -> spare channel (kept OFF; free for future use)

#define STATUS_LED_PIN         2     // onboard LED on most ESP32 DevKit boards
#define BOOT_BUTTON_PIN        0     // the "BOOT" button on the board

// Most 4-channel relay modules switch ON when the IN pin is driven LOW
// ("active-low"). If yours turns ON when driven HIGH, set this to false.
// Brassica_Diagnostics.ino lets you check this safely before anything is wired to the relays.
#define RELAY_ACTIVE_LOW     true

// ============================================================
// Calibration — REQUIRED before trusting the readings
// ============================================================
// Run Brassica_Diagnostics.ino first and read the raw ADC numbers (0-4095)
// of EACH soil sensor: in dry air -> *_RAW_DRY (reported as 0%), fully in
// water -> *_RAW_WET (reported as 100%). Two sensors of the same model can
// read quite differently, so each one has its own pair of numbers.
#define SOIL1_RAW_DRY         3500
#define SOIL1_RAW_WET         1400
#define SOIL2_RAW_DRY         3500
#define SOIL2_RAW_WET         1400

// ============================================================
// Automation thresholds
// ============================================================
// Keep in sync with the dashboard texts (public/js/ui.js).
//
// Exhaust fans (temperature). Both default to the same values so they run
// together. To run them in stages, give fan 2 a HIGHER temperature than fan 1
// (for example fan 1 at 30 C and fan 2 at 33 C).
#define TEMP_FAN1_ON_C         30.0   // fan 1 ON at/above this
#define TEMP_FAN1_OFF_C        29.9   // fan 1 OFF at/below this
#define TEMP_FAN2_ON_C         30.0   // fan 2 ON at/above this
#define TEMP_FAN2_OFF_C        29.9   // fan 2 OFF at/below this

// Pump (soil moisture).
#define SOIL_PUMP_ON_PCT        40    // pump ON at/below this
#define SOIL_PUMP_OFF_PCT       55    // pump OFF at/above this

// Which soil value drives the pump when both sensors work:
//   0 = the AVERAGE of the two sensors (default)
//   1 = the DRIEST sensor (waters until even the dry spot is wet)
// If only one sensor is working, that one is used. If both fail, the pump is
// forced OFF.
#define SOIL_CONTROL_MODE 1     // 0 = average, 1 = driest
// ============================================================
// Manual override
// ============================================================
// When the dashboard's "Water Pump Override" button is pressed, the pump
// is driven by the dashboard (ON until the user turns it OFF) and the
// automatic soil logic is ignored. If the ESP32 reboots, or loses the
// server for longer than MANUAL_OVERRIDE_TIMEOUT_MS, the override is
// dropped and automation resumes — the pump can never be left running
// forever by a stale command.
#define MANUAL_OVERRIDE_TIMEOUT_MS   120000UL   // 2 min without confirmation = drop override
// ============================================================
// Relay protection
// ============================================================
// After any relay changes state it will not be switched again by a
// threshold for this long. Stops rapid clicking that wears relays and
// resets/brownouts the ESP32. (Fail-safe OFF is always immediate.)
#define MIN_RELAY_HOLD_MS     30000

// Pump safety: a pump never runs longer than PUMP_MAX_RUN_MS in one go,
// then it rests for PUMP_COOLDOWN_MS so water can soak in to the soil
// sensor. Protects against a stuck/dry sensor flooding the plant or
// running the pump dry. Raise PUMP_MAX_RUN_MS if your pot needs more water.
#define PUMP_MAX_RUN_MS       30000   // 30 s
#define PUMP_COOLDOWN_MS     60000   // 1 min

// ============================================================
// Timing (milliseconds)
// ============================================================
#define SENSOR_READ_INTERVAL_MS     2500   // DHT11 needs >= 2 s between reads

// How often a reading is sent to the dashboard.
//   Hosted (HTTPS, e.g. Vercel): every 10 s. The free Redis plan behind a Vercel
//   deployment allows 500,000 commands a month; 10 s keeps you safely under it
//   (5 s would use it all up in ~2 weeks, after which the dashboard flickers).
//   Local (plain HTTP to desktop.py): every 3 s, there is no quota on your own PC.
#define SERVER_POST_INTERVAL_HOSTED_MS   10000
#define SERVER_POST_INTERVAL_LOCAL_MS     3000

// How often the ESP32 asks the server whether a Wi-Fi change / rescan was
// requested from the dashboard (this is the delay before a change you make
// on the website reaches the device).
#define CTL_CHECK_INTERVAL_HOSTED_MS     30000
#define CTL_CHECK_INTERVAL_LOCAL_MS       6000

// How often a row is added to the history used by the CSV/Excel export
// and the 7-day averages.
#define HISTORY_LOG_INTERVAL_MS         300000
#define HTTP_CONNECT_TIMEOUT_MS     6000
#define HTTP_RESPONSE_TIMEOUT_MS    8000   // allows a cold Vercel start + database round-trip
#define MIN_HEAP_FOR_TLS           40000   // skip a post if the largest free block is smaller

// ============================================================
// Wi-Fi behaviour
// ============================================================
#define WIFI_ATTEMPT_MS            15000   // give each saved network this long before trying the next
#define WIFI_REBOOT_AFTER_MS      600000   // reboot if Wi-Fi has been down this long (0 = never)
#define WIFI_TRIAL_TIMEOUT_MS      45000   // a network chosen on the dashboard must work within this
                                           // time, otherwise the ESP32 returns to its previous network
#define WIFI_SCAN_INTERVAL_MS     300000   // refresh the list of nearby networks this often (a rescan
                                           // can also be requested from the dashboard at any time)
#define MAX_SCAN_NETWORKS             10   // how many nearby networks are shown on the dashboard

// ============================================================
// Setup portal ("Brassica-Setup-XXXX" Wi-Fi) — the no-reflash fallback
// ============================================================
// Opens automatically when Wi-Fi has been down for PORTAL_AFTER_WIFI_DOWN_MS,
// or when the dashboard server has been unreachable for PORTAL_AFTER_POST_FAILS
// posts in a row (about 4 minutes when hosted, 1 minute locally). You can also open it by holding the BOOT button for
// 3 seconds. Holding BOOT for 10 seconds erases all saved settings.
#define PORTAL_AP_PREFIX            "Brassica-Setup"
#define PORTAL_AP_PASSWORD          "brassica123"   // 8+ characters, or "" for an open network
#define PORTAL_AFTER_WIFI_DOWN_MS  60000
#define PORTAL_AFTER_POST_FAILS       24

// ============================================================
// Sensor fault detection
// ============================================================
#define DHT_FAIL_THRESHOLD           2   // consecutive DHT failures before "disconnected"
#define RAIL_COUNT_THRESHOLD         2   // consecutive rail-pinned analog reads before a soil sensor is "disconnected"
#define ADC_SAMPLES                 16   // analog reads averaged per measurement (noise filter)
#define SOIL_NOISE_LIMIT 300   // tune this: disconnected pins usually jump >100 counts

// ============================================================
// Sensor fault detection
// ============================================================

#endif // BRASSICA_CONFIG_H
