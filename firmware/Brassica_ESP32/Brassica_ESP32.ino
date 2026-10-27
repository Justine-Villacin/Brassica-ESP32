/*
  Brassica_ESP32.ino  (v6.1)
  ESP32 greenhouse controller for the Brassica dashboard.

  HARDWARE
    1 x DHT11 (temperature / humidity)
    2 x soil moisture sensors (analog)
    2 x 12V exhaust fans  + 1 x 12V water pump, switched by a 4-channel relay:
      IN1 = pump, IN2 = exhaust fan 2, IN3 = exhaust fan 1, IN4 = spare.
    All pins are in config.h.

  WHAT IT DOES
    - Reads the sensors and runs the automation LOCALLY (works with no Wi-Fi):
        fans  <- temperature      (each fan has its own thresholds)
        pump  <- soil moisture    (either of two)
    - FAIL-SAFE: if a sensor looks disconnected, whatever depends on it is forced
      OFF immediately. One soil sensor failing is fine (the other one is used);
      both failing switches the pump off.
    - Pump safety: max run time + cooldown, so it can never run away.
    - Reports to the dashboard with POST /update (every 10 s hosted, 3 s local).
    - Wi-Fi self-heals; the sensor/automation loop is never blocked by Wi-Fi.

  CHANGE Wi-Fi WITHOUT RE-FLASHING (since v6.0)
    1. The ESP32 scans for nearby networks and sends the list to the dashboard.
       Pick one there, type the password, and the ESP32 switches over. If the new
       network does not work within 45 s it goes back to the old one by itself
       and tells the dashboard what went wrong.
    2. If it cannot connect to ANY network (or cannot reach the dashboard server
       for a few minutes) it opens its own Wi-Fi, "Brassica-Setup-XXXX"
       (password in config.h). Join it with a phone, open http://192.168.4.1 and
       type the network (and, if needed, the server address).
    3. BOOT button: hold 3 s = open that setup Wi-Fi now. Hold 10 s = erase all
       saved settings.
    Saved settings live in flash and survive reboots. config.h only holds the
    defaults used when nothing has been saved.

  NEW IN v6.1
    - Second soil sensor and second exhaust fan; the light sensor and warm light
      were removed.

  JSON SENT TO POST /update
    temperature, humidity         (omitted when the DHT is down)
    soil_1, soil_2                (each omitted when that sensor is down)
    soil_moisture                 (average of the working soil sensors)
    dht_connected, soil1_connected, soil2_connected
    fan, fan2, water_pump         ("ON"/"OFF"; fan = exhaust fan 1)
    wifi_ssid, ip, rssi, fw       (device info)
    ctl                           (true about every 30 s: "any Wi-Fi change waiting for me?")
    log                           (true about every 5 min: "store a history row")
    wifi_networks                 (only right after a new scan)
    wifi_ack                      (only while reporting the result of a Wi-Fi change)

  LIBRARIES (Arduino IDE -> Tools -> Manage Libraries)
    - "DHT sensor library" by Adafruit (accept "Adafruit Unified Sensor" too)
    Everything else (WiFi, HTTPClient, Preferences, WebServer, DNSServer) is
    built into the "esp32" board package by Espressif (Boards Manager).
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <DHT.h>
#include "config.h"

DHT dht(DHT_PIN, DHT_TYPE);
Preferences prefs;
WebServer portalServer(80);
DNSServer dnsServer;

// ---- Live sensor/relay state ----
float temperatureC = 0.0f;
float humidityPct  = 0.0f;
int   soil1Pct     = 0;
int   soil2Pct     = 0;
int   soilAvgPct   = 0;     // average of the working soil sensors (reported to the dashboard)
int   soilCtlPct   = 0;     // the value that drives the pump (average or driest, see config.h)

bool dhtConnected   = false;
bool soil1Connected = false;
bool soil2Connected = false;
bool haveSoil       = false;   // at least one soil sensor works

bool pumpOn = false;
bool fan1On = false;
bool fan2On = false;

// valid = we have received at least one good reading since boot / recovery
bool soil1Valid = false;
bool soil2Valid = false;

int dhtFailCount   = 0;
int soil1RailCount = 0;
int soil2RailCount = 0;

// Time each relay last changed. Initialised "long ago" so the first
// switch after boot is allowed immediately (unsigned wrap-around is intended).
unsigned long fan1ChangedAt = (unsigned long)(0UL - (unsigned long)MIN_RELAY_HOLD_MS);
unsigned long fan2ChangedAt = (unsigned long)(0UL - (unsigned long)MIN_RELAY_HOLD_MS);
unsigned long pumpChangedAt = (unsigned long)(0UL - (unsigned long)MIN_RELAY_HOLD_MS);

bool pumpInCooldown = false;
unsigned long pumpCooldownStart = 0;

unsigned long lastSensorRead = 0;
unsigned long lastServerPost = 0;
unsigned long lastPostOkAt   = 0;
unsigned long lastCtlOkAt    = 0;    // last successful post that also asked for pending commands
unsigned long lastLogOkAt    = 0;    // last successful post that also added a history row
bool ctlEverSent = false;
bool logEverSent = false;
int lowHeapCount  = 0;
int postFailCount = 0;

// ---- Server target: defaults from config.h, can be overridden and saved ----
String srvHost = SERVER_HOST;
int    srvPort = SERVER_PORT;
bool   srvTls  = SERVER_USE_HTTPS;

// ---- Wi-Fi supervision ----
String candSsid[2];
String candPass[2];
int candCount = 0;
int candIndex = 0;
unsigned long connectingSince = 0;   // 0 = no attempt running
unsigned long wifiDownSince   = 0;   // 0 = Wi-Fi currently up
volatile int lastDiscReason   = 0;

// ---- Wi-Fi change requested from the dashboard ("trial") ----
bool   trialActive    = false;
bool   trialConnected = false;
String trialId, trialSsid, trialPass;
unsigned long trialStart = 0;
String handledId;                    // last request id we finished (ok or failed)

// result to report back to the dashboard (sent until a POST succeeds)
String ackId, ackStatus, ackSsid, ackReason;

// request received in a reply, applied after the HTTP call has finished
bool   havePend = false;
String pendId, pendSsid, pendPass;

// ---- Nearby-network scan (asynchronous) ----
bool scanRunning   = false;
bool scanRequested = true;           // first scan as soon as Wi-Fi is up
bool networksFresh = false;          // new list not yet sent to the dashboard
unsigned long scanStartedAt = 0;
unsigned long lastScanAt    = 0;
String networksJson = "[]";
String networksHtml = "";

// ---- Setup portal ----
bool portalActive = false;
String portalSsid;
unsigned long portalStartedAt  = 0;
unsigned long portalLastClient = 0;
unsigned long rebootAt = 0;

// ---- BOOT button ----
unsigned long buttonDownAt = 0;
int  buttonStage = 0;
bool buttonPortalRequested = false;

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
void setRelay(uint8_t pin, bool on);
void writeAllRelays();
String jsonEscape(const String& s);
String htmlEscape(const String& s);
bool isValidUtf8(const String& s);
String nvsGetString(const char* key);
void nvsPutString(const char* key, const String& value);
void nvsRemove(const char* key);
void loadSettings();
void buildCandidates();
void beginWiFi(const String& ssid, const String& pass);
void startWiFi();
void maintainWiFi(unsigned long now);
void serviceTrial(unsigned long now, bool up);
void startTrial();
void commitTrial();
void failTrial(const String& reason);
void setAck(const String& id, const char* status, const String& ssid, const String& reason);
String disconnectReasonText(int r);
unsigned long postIntervalMs();
unsigned long ctlIntervalMs();
void startScan();
void finishScan(int n);
void serviceScan(unsigned long now);
void startPortal();
void stopPortal();
void servicePortal(unsigned long now);
void handlePortalRoot();
void handlePortalSave();
void handlePortalRedirect();
void handleButton(unsigned long now);
void factoryReset();
void readSensors();
void runAutomation(unsigned long now);
String buildPayload(bool wantCtl, bool wantLog);
bool postReading();
void handleReply(const String& body);
void printStatus();

// ---------------------------------------------------------------------------
// Relay helper: handles active-LOW / active-HIGH boards
// ---------------------------------------------------------------------------
void setRelay(uint8_t pin, bool on) {
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(pin, level ? HIGH : LOW);
}

void writeAllRelays() {
  setRelay(RELAY_PUMP_PIN,  pumpOn);
  setRelay(RELAY_FAN1_PIN,  fan1On);
  setRelay(RELAY_FAN2_PIN,  fan2On);
  setRelay(RELAY_SPARE_PIN, false);   // spare channel always OFF
}

// ---------------------------------------------------------------------------
// Timing depends on where the dashboard lives (see config.h)
// ---------------------------------------------------------------------------
unsigned long postIntervalMs() {
  return srvTls ? (unsigned long)SERVER_POST_INTERVAL_HOSTED_MS : (unsigned long)SERVER_POST_INTERVAL_LOCAL_MS;
}

unsigned long ctlIntervalMs() {
  return srvTls ? (unsigned long)CTL_CHECK_INTERVAL_HOSTED_MS : (unsigned long)CTL_CHECK_INTERVAL_LOCAL_MS;
}

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------
String jsonEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"')       out += "\\\"";
    else if (c == '\\') out += "\\\\";
    else if ((unsigned char)c < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
      out += buf;
    } else {
      out += c;
    }
  }
  return out;
}

String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (unsigned int i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&')       out += "&amp;";
    else if (c == '<')  out += "&lt;";
    else if (c == '>')  out += "&gt;";
    else if (c == '"')  out += "&quot;";
    else if (c == '\'') out += "&#39;";
    else                out += c;
  }
  return out;
}

// A neighbour's Wi-Fi name can contain any bytes. Anything that is not valid
// UTF-8 is skipped so it can never corrupt the JSON we send.
bool isValidUtf8(const String& s) {
  const unsigned char* p = (const unsigned char*)s.c_str();
  size_t n = s.length();
  size_t i = 0;
  while (i < n) {
    unsigned char c = p[i];
    size_t extra;
    if (c < 0x80)                           extra = 0;
    else if (c >= 0xC2 && c <= 0xDF)        extra = 1;
    else if (c >= 0xE0 && c <= 0xEF)        extra = 2;
    else if (c >= 0xF0 && c <= 0xF4)        extra = 3;
    else return false;
    if (i + extra >= n && extra > 0) return false;
    for (size_t k = 1; k <= extra; k++) {
      if ((p[i + k] & 0xC0) != 0x80) return false;
    }
    i += extra + 1;
  }
  return true;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// "4a6f" -> "Jo". Returns "" for odd length, non-hex, or an embedded NUL.
String hexDecode(const String& hex) {
  String out;
  if (hex.length() % 2 != 0) return out;
  for (unsigned int i = 0; i + 1 < hex.length(); i += 2) {
    int hi = hexVal(hex[i]);
    int lo = hexVal(hex[i + 1]);
    if (hi < 0 || lo < 0) return String("");
    int v = (hi << 4) | lo;
    if (v == 0) return String("");
    out += (char)v;
  }
  return out;
}

// Finds  "key" : "value"  in a small JSON text and returns value ("" if absent).
// Only used on the server's reply, whose values are plain hex / letters.
String jsonStringValue(const String& body, const char* key) {
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return String("");
  int i = k + (int)needle.length();
  int len = (int)body.length();
  while (i < len && (body[i] == ' ' || body[i] == ':')) i++;
  if (i >= len || body[i] != '"') return String("");
  i++;
  int j = body.indexOf('"', i);
  if (j < 0) return String("");
  return body.substring(i, j);
}

bool jsonBoolTrue(const String& body, const char* key) {
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return false;
  int i = k + (int)needle.length();
  int len = (int)body.length();
  while (i < len && (body[i] == ' ' || body[i] == ':')) i++;
  return body.substring(i, i + 4) == "true";
}

// ---------------------------------------------------------------------------
// Saved settings (flash / NVS)
// ---------------------------------------------------------------------------
String nvsGetString(const char* key) {
  String v = "";
  prefs.begin("brassica", false);
  if (prefs.isKey(key)) v = prefs.getString(key, "");
  prefs.end();
  return v;
}

void nvsPutString(const char* key, const String& value) {
  prefs.begin("brassica", false);
  prefs.putString(key, value);
  prefs.end();
}

void nvsRemove(const char* key) {
  prefs.begin("brassica", false);
  if (prefs.isKey(key)) prefs.remove(key);
  prefs.end();
}

void loadSettings() {
  String h = nvsGetString("host");
  if (h.length() > 0) {
    srvHost = h;
    prefs.begin("brassica", false);
    srvPort = (int)prefs.getUShort("port", (uint16_t)SERVER_PORT);
    srvTls  = prefs.getBool("tls", SERVER_USE_HTTPS);
    prefs.end();
  }
  handledId = nvsGetString("cfgid");
}

// Saved network first, then the defaults from config.h. A bad saved network
// can therefore never strand the device.
void buildCandidates() {
  candCount = 0;
  String saved = nvsGetString("ssid");
  if (saved.length() > 0) {
    candSsid[candCount] = saved;
    candPass[candCount] = nvsGetString("pass");
    candCount++;
  }
  String def = WIFI_SSID;
  if (def.length() > 0 && def != saved) {
    candSsid[candCount] = def;
    candPass[candCount] = WIFI_PASSWORD;
    candCount++;
  }
  if (candIndex >= candCount) candIndex = 0;
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      int reason = (int)info.wifi_sta_disconnected.reason;
      // 8 = "we left on purpose" (our own disconnect before a new begin()).
      if (reason != 8) lastDiscReason = reason;
      Serial.printf("[WiFi] Disconnected, reason code %d\n", reason);
      break;
    }
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      lastDiscReason = 0;
      Serial.printf("[WiFi] Connected. IP %s  RSSI %d dBm\n",
                    WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
      break;
    default:
      break;
  }
}

String disconnectReasonText(int r) {
  switch (r) {
    case 201: return String("network not found (is it 2.4 GHz and in range?)");
    case 2:
    case 15:
    case 202:
    case 204: return String("wrong password?");
    case 203: return String("the router refused the connection");
    case 200: return String("signal too weak");
    default:  return String("could not connect");
  }
}

void beginWiFi(const String& ssid, const String& pass) {
  WiFi.disconnect(false);
  if (pass.length() > 0) WiFi.begin(ssid.c_str(), pass.c_str());
  else                   WiFi.begin(ssid.c_str());
  connectingSince = millis();
  if (connectingSince == 0) connectingSince = 1;
}

void startWiFi() {
  WiFi.persistent(false);          // don't wear out flash on every begin()
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);            // modem-sleep causes random dropouts/latency
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(onWiFiEvent);
  if (candCount > 0) {
    candIndex = 0;
    beginWiFi(candSsid[0], candPass[0]);
    Serial.printf("[Brassica] Wi-Fi connecting to \"%s\" in background...\n", candSsid[0].c_str());
  } else {
    Serial.println("[Brassica] No Wi-Fi configured, the setup Wi-Fi will open.");
  }
}

// Wi-Fi supervision (never blocks)
void maintainWiFi(unsigned long now) {
  bool up = (WiFi.status() == WL_CONNECTED);

  if (trialActive) {               // a network chosen on the dashboard is being tested
    serviceTrial(now, up);
    return;
  }

  if (up) {
    wifiDownSince = 0;
    connectingSince = 0;
    return;
  }

  if (wifiDownSince == 0) wifiDownSince = (now == 0) ? 1 : now;

#if WIFI_REBOOT_AFTER_MS > 0
  if (now - wifiDownSince >= (unsigned long)WIFI_REBOOT_AFTER_MS &&
      (!portalActive || WiFi.softAPgetStationNum() == 0)) {
    Serial.println("[Brassica] Wi-Fi down too long, rebooting to recover the radio.");
    delay(100);
    ESP.restart();
  }
#endif

  if (candCount == 0) return;      // nothing to try; the setup portal handles this

  if (connectingSince == 0 || now - connectingSince >= (unsigned long)WIFI_ATTEMPT_MS) {
    if (connectingSince != 0) candIndex = (candIndex + 1) % candCount;   // try the next network
    Serial.printf("[Brassica] Wi-Fi down, trying \"%s\"...\n", candSsid[candIndex].c_str());
    beginWiFi(candSsid[candIndex], candPass[candIndex]);
  }
}

// ---- Wi-Fi change requested from the dashboard ----
void setAck(const String& id, const char* status, const String& ssid, const String& reason) {
  ackId = id;
  ackStatus = status;
  ackSsid = ssid;
  ackReason = reason;
}

void startTrial() {
  if (trialActive) return;
  if (pendId.length() == 0 || pendId == handledId) return;   // already dealt with

  trialActive = true;
  trialConnected = false;
  trialId = pendId;
  trialSsid = pendSsid;
  trialPass = pendPass;
  trialStart = millis();
  lastDiscReason = 0;
  Serial.printf("[Brassica] Dashboard asked to join \"%s\". Trying it (old network is kept as a fallback)...\n",
                trialSsid.c_str());
  beginWiFi(trialSsid, trialPass);
}

void commitTrial() {
  nvsPutString("ssid", trialSsid);
  nvsPutString("pass", trialPass);
  nvsPutString("cfgid", trialId);
  handledId = trialId;
  trialActive = false;
  trialConnected = false;
  candIndex = 0;
  buildCandidates();
  Serial.printf("[Brassica] Wi-Fi \"%s\" confirmed and saved.\n", trialSsid.c_str());
}

void failTrial(const String& reason) {
  Serial.printf("[Brassica] Could not use \"%s\": %s. Going back to the previous network.\n",
                trialSsid.c_str(), reason.c_str());
  setAck(trialId, "failed", trialSsid, reason);
  handledId = trialId;
  nvsPutString("cfgid", trialId);
  trialActive = false;
  trialConnected = false;
  candIndex = 0;
  buildCandidates();
  if (candCount > 0) beginWiFi(candSsid[0], candPass[0]);
  else               WiFi.disconnect(false);
}

void serviceTrial(unsigned long now, bool up) {
  if (up && WiFi.SSID() == trialSsid) {
    if (!trialConnected) {
      trialConnected = true;
      setAck(trialId, "ok", trialSsid, String(""));
      lastServerPost = millis() - postIntervalMs();            // report right away
      Serial.println("[Brassica] Joined the new network, confirming with the dashboard...");
    }
  } else if (!up && connectingSince != 0 && now - connectingSince >= (unsigned long)WIFI_ATTEMPT_MS) {
    beginWiFi(trialSsid, trialPass);                          // one more try
  }

  if (now - trialStart >= (unsigned long)WIFI_TRIAL_TIMEOUT_MS) {
    if (trialConnected) failTrial(String("connected, but the dashboard server could not be reached on that network"));
    else                failTrial(disconnectReasonText(lastDiscReason));
  }
}

// ---------------------------------------------------------------------------
// Nearby networks (asynchronous: never blocks sensors or relays)
// ---------------------------------------------------------------------------
void finishScan(int n) {
  String json = "[";
  String html = "";
  String seen[MAX_SCAN_NETWORKS];
  int count = 0;
  for (int i = 0; i < n && count < MAX_SCAN_NETWORKS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0 || ssid.length() > 32 || !isValidUtf8(ssid)) continue;
    bool dup = false;
    for (int k = 0; k < count; k++) {
      if (seen[k] == ssid) { dup = true; break; }
    }
    if (dup) continue;
    seen[count] = ssid;
    if (count > 0) json += ",";
    json += "{\"ssid\":\"";
    json += jsonEscape(ssid);
    json += "\",\"rssi\":";
    json += String((int)WiFi.RSSI(i));
    json += ",\"secure\":";
    json += (WiFi.encryptionType(i) != WIFI_AUTH_OPEN) ? "true" : "false";
    json += "}";
    html += "<option value=\"";
    html += htmlEscape(ssid);
    html += "\">";
    count++;
  }
  json += "]";
  networksJson = json;
  networksHtml = html;
  networksFresh = true;
  WiFi.scanDelete();
  lastScanAt = millis();
  Serial.printf("[Brassica] Scan done: %d nearby networks listed.\n", count);
}

void startScan() {
  if (scanRunning) return;
  scanRequested = false;
  lastScanAt = millis();           // never retry in a tight loop, even if this fails
  WiFi.scanDelete();
  int r = WiFi.scanNetworks(true, true);   // async = true, show hidden = true
  if (r == WIFI_SCAN_RUNNING) {
    scanRunning = true;
    scanStartedAt = millis();
  } else if (r >= 0) {
    finishScan(r);
  } else {
    Serial.println("[Brassica] Wi-Fi scan could not start, will retry later.");
  }
}

void serviceScan(unsigned long now) {
  if (scanRunning) {
    int n = WiFi.scanComplete();
    if (n >= 0) {
      scanRunning = false;
      finishScan(n);
    } else if (n == WIFI_SCAN_FAILED || now - scanStartedAt > 15000UL) {
      scanRunning = false;
      WiFi.scanDelete();
      Serial.println("[Brassica] Wi-Fi scan failed.");
    }
    return;
  }
  if (trialActive) return;                                  // never scan while testing a network
  if (WiFi.status() != WL_CONNECTED && !portalActive) return;
  if (scanRequested || now - lastScanAt >= (unsigned long)WIFI_SCAN_INTERVAL_MS) startScan();
}

// ---------------------------------------------------------------------------
// Setup portal  (the ESP32's own Wi-Fi, for when it cannot connect anywhere)
// ---------------------------------------------------------------------------
void handlePortalRedirect() {
  portalServer.sendHeader("Location", String("http://") + WiFi.softAPIP().toString() + "/", true);
  portalServer.send(302, "text/plain", "");
}

void handlePortalRoot() {
  String saved = nvsGetString("ssid");
  String html;
  html.reserve(3200);
  html += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
          "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
          "<title>Brassica Setup</title><style>"
          "body{margin:0;background:#030a07;color:#e8f5ee;font-family:system-ui,Arial,sans-serif}"
          ".c{max-width:420px;margin:0 auto;padding:24px}"
          "h1{color:#00ff88;font-size:24px;margin:0 0 4px}"
          "p{color:#9bb5a8;font-size:14px;line-height:1.5}"
          "label{display:block;margin:16px 0 6px;font-size:13px;color:#9bb5a8}"
          "input[type=text],input[type=password],input[type=number]{width:100%;box-sizing:border-box;padding:12px;"
          "border-radius:8px;border:1px solid #2a3d33;background:#0b1812;color:#fff;font-size:16px}"
          "button{margin-top:22px;width:100%;padding:14px;border:0;border-radius:8px;background:#00ff88;"
          "color:#021;font-size:16px;font-weight:700}"
          "details{margin-top:18px;border-top:1px solid #1d2d25;padding-top:12px}summary{color:#9bb5a8;font-size:14px}"
          ".s{font-size:12px;color:#6f8a7c;margin-top:18px}"
          "</style></head><body><div class=\"c\"><h1>Brassica Setup</h1>"
          "<p>Choose the Wi-Fi network this device should use (2.4 GHz only).</p>"
          "<form method=\"POST\" action=\"/save\">"
          "<label>Wi-Fi name</label>"
          "<input type=\"text\" name=\"ssid\" list=\"nets\" maxlength=\"32\" autocomplete=\"off\" autocapitalize=\"none\" value=\"";
  html += htmlEscape(saved);
  html += "\"><datalist id=\"nets\">";
  html += networksHtml;
  html += "</datalist><label>Wi-Fi password (empty for an open network)</label>"
          "<input type=\"password\" name=\"pass\" maxlength=\"63\" autocomplete=\"off\">"
          "<details><summary>Dashboard server (advanced)</summary>"
          "<label>Server address (leave empty for the default)</label>"
          "<input type=\"text\" name=\"host\" maxlength=\"100\" autocapitalize=\"none\" placeholder=\"192.168.1.53 or brassica.vercel.app\" value=\"";
  html += htmlEscape(srvHost);
  html += "\"><label>Port</label><input type=\"number\" name=\"port\" min=\"1\" max=\"65535\" value=\"";
  html += String(srvPort);
  html += "\"><label><input type=\"checkbox\" name=\"tls\" value=\"1\"";
  if (srvTls) html += " checked";
  html += "> Use HTTPS (needed for *.vercel.app)</label></details>"
          "<button type=\"submit\">Save and restart</button></form>"
          "<p class=\"s\">Currently: ";
  if (WiFi.status() == WL_CONNECTED) {
    html += "connected to ";
    html += htmlEscape(WiFi.SSID());
  } else {
    html += "not connected to any network";
  }
  html += " &middot; firmware " FIRMWARE_VERSION "</p></div></body></html>";
  portalServer.send(200, "text/html", html);
}

void handlePortalSave() {
  String ssid = portalServer.arg("ssid");
  String pass = portalServer.arg("pass");
  String host = portalServer.arg("host");
  int port = portalServer.arg("port").toInt();
  bool tls = portalServer.hasArg("tls");

  host.trim();
  if (host.startsWith("https://")) host = host.substring(8);
  else if (host.startsWith("http://")) host = host.substring(7);
  while (host.endsWith("/")) host = host.substring(0, host.length() - 1);

  String error = "";
  if (ssid.length() > 32) error = "The Wi-Fi name is too long (max 32).";
  else if (ssid.length() == 0 && nvsGetString("ssid").length() == 0 && candCount == 0) error = "Please enter a Wi-Fi name.";
  else if (pass.length() > 0 && (pass.length() < 8 || pass.length() > 63)) error = "Wi-Fi passwords are 8-63 characters.";
  else if (host.length() > 100 || host.indexOf(' ') >= 0) error = "That server address does not look right.";

  if (error.length() > 0) {
    portalServer.send(400, "text/html",
      "<html><body style=\"font-family:sans-serif;background:#030a07;color:#fff;padding:24px\">"
      "<h2>Not saved</h2><p>" + htmlEscape(error) + "</p><p><a style=\"color:#00ff88\" href=\"/\">Go back</a></p></body></html>");
    return;
  }

  if (ssid.length() > 0) {
    nvsPutString("ssid", ssid);
    nvsPutString("pass", pass);
  }
  if (host.length() > 0) {
    if (port < 1 || port > 65535) port = tls ? 443 : 8000;
    nvsPutString("host", host);
    prefs.begin("brassica", false);
    prefs.putUShort("port", (uint16_t)port);
    prefs.putBool("tls", tls);
    prefs.end();
  } else {
    nvsRemove("host");
    prefs.begin("brassica", false);
    if (prefs.isKey("port")) prefs.remove("port");
    if (prefs.isKey("tls"))  prefs.remove("tls");
    prefs.end();
  }

  portalServer.send(200, "text/html",
    "<html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"></head>"
    "<body style=\"font-family:sans-serif;background:#030a07;color:#fff;padding:24px\">"
    "<h2 style=\"color:#00ff88\">Saved</h2><p>The device is restarting and will join your network. "
    "You can close this page and reconnect your phone to your normal Wi-Fi.</p></body></html>");
  Serial.println("[Brassica] Settings saved from the setup page, restarting...");
  rebootAt = millis() + 2500;
  if (rebootAt == 0) rebootAt = 1;
}

void startPortal() {
  if (portalActive) return;
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  portalSsid = String(PORTAL_AP_PREFIX) + "-" + mac.substring(mac.length() - 4);

  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  bool ok;
  if (strlen(PORTAL_AP_PASSWORD) >= 8) ok = WiFi.softAP(portalSsid.c_str(), PORTAL_AP_PASSWORD);
  else                                 ok = WiFi.softAP(portalSsid.c_str());
  if (!ok) {
    Serial.println("[Brassica] Could not start the setup Wi-Fi.");
    return;
  }
  delay(100);
  dnsServer.start(53, "*", WiFi.softAPIP());   // makes phones open the page automatically
  portalServer.begin();
  portalActive = true;
  portalStartedAt = millis();
  portalLastClient = portalStartedAt;
  scanRequested = true;
  Serial.printf("[Brassica] Setup Wi-Fi \"%s\" is open%s. Join it and visit http://%s\n",
                portalSsid.c_str(),
                strlen(PORTAL_AP_PASSWORD) >= 8 ? " (password: " PORTAL_AP_PASSWORD ")" : "",
                WiFi.softAPIP().toString().c_str());
}

void stopPortal() {
  if (!portalActive) return;
  dnsServer.stop();
  portalServer.stop();
  WiFi.softAPdisconnect(true);
  if (WiFi.getMode() != WIFI_STA) WiFi.mode(WIFI_STA);
  portalActive = false;
  Serial.println("[Brassica] Setup Wi-Fi closed (connection is healthy again).");
}

void servicePortal(unsigned long now) {
  bool up = (WiFi.status() == WL_CONNECTED);

  if (!portalActive) {
    bool want = false;
    if (candCount == 0 && !trialActive) want = true;                                   // nothing configured
    if (!up && !trialActive && wifiDownSince != 0 &&
        now - wifiDownSince >= (unsigned long)PORTAL_AFTER_WIFI_DOWN_MS) want = true;  // Wi-Fi gone
    if (up && postFailCount >= PORTAL_AFTER_POST_FAILS) want = true;                   // server unreachable
    if (buttonPortalRequested) { want = true; buttonPortalRequested = false; }         // BOOT held 3 s
    if (want) startPortal();
    return;
  }

  dnsServer.processNextRequest();
  portalServer.handleClient();

  if (WiFi.softAPgetStationNum() > 0) portalLastClient = now;

  // Close it again once everything works and nobody has used it for 2 minutes.
  if (up && !trialActive && postFailCount == 0 && lastPostOkAt != 0 &&
      now - lastPostOkAt < 30000UL && now - portalLastClient > 120000UL) {
    stopPortal();
  }
}

// ---------------------------------------------------------------------------
// BOOT button: hold 3 s = setup Wi-Fi, hold 10 s = erase saved settings
// ---------------------------------------------------------------------------
void factoryReset() {
  Serial.println("[Brassica] Erasing all saved settings and restarting...");
  prefs.begin("brassica", false);
  prefs.clear();
  prefs.end();
  delay(300);
  ESP.restart();
}

void handleButton(unsigned long now) {
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    if (buttonDownAt == 0) {
      buttonDownAt = (now == 0) ? 1 : now;
      buttonStage = 0;
    }
    unsigned long held = now - buttonDownAt;
    if (held >= 10000UL && buttonStage < 2) {
      buttonStage = 2;
      factoryReset();
    } else if (held >= 3000UL && buttonStage < 1) {
      buttonStage = 1;
      buttonPortalRequested = true;
      Serial.println("[Brassica] BOOT held: opening the setup Wi-Fi (keep holding 10 s to erase settings).");
    }
  } else {
    buttonDownAt = 0;
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[Brassica] Booting v" FIRMWARE_VERSION "...");

  pinMode(RELAY_PUMP_PIN, OUTPUT);
  pinMode(RELAY_FAN1_PIN, OUTPUT);
  pinMode(RELAY_FAN2_PIN, OUTPUT);
  pinMode(RELAY_SPARE_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  writeAllRelays();                // everything OFF until a real reading says otherwise

  pinMode(SOIL1_PIN, INPUT);
  pinMode(SOIL2_PIN, INPUT);
  dht.begin();

  prefs.begin("brassica", false);  // creates the settings area on first boot
  prefs.end();
  loadSettings();
  buildCandidates();

  portalServer.on("/", HTTP_GET, handlePortalRoot);
  portalServer.on("/save", HTTP_POST, handlePortalSave);
  portalServer.onNotFound(handlePortalRedirect);

  Serial.printf("[Brassica] Server target: %s://%s:%d%s\n",
                srvTls ? "https" : "http", srvHost.c_str(), srvPort, UPDATE_PATH);
  startWiFi();                     // non-blocking: automation starts immediately
}

// ---------------------------------------------------------------------------
void loop() {
  unsigned long now = millis();

  handleButton(now);
  maintainWiFi(now);
  serviceScan(now);
  servicePortal(now);

  if (now - lastSensorRead >= SENSOR_READ_INTERVAL_MS) {
    lastSensorRead = now;
    readSensors();
    runAutomation(millis());
    printStatus();
  }

  // Never post while a scan is running: the radio is busy and the post would be slow.
  if (!scanRunning && now - lastServerPost >= postIntervalMs()) {
    if (WiFi.status() == WL_CONNECTED) {
      postReading();
    } else {
      Serial.println("[Brassica] Skipping post, Wi-Fi not connected.");
    }
    lastServerPost = millis();     // measured AFTER the post so a slow post never causes back-to-back posts
  }

  if (rebootAt != 0 && (long)(millis() - rebootAt) >= 0) {
    ESP.restart();
  }

  // Status LED: solid = Wi-Fi up, slow blink = Wi-Fi down, fast blink = setup Wi-Fi open.
  unsigned long blinkMs = portalActive ? 150UL : 300UL;
  bool wifiUp = (WiFi.status() == WL_CONNECTED);
  digitalWrite(STATUS_LED_PIN, (wifiUp && !portalActive) ? HIGH : (((millis() / blinkMs) % 2) ? HIGH : LOW));

  delay(portalActive ? 5 : 20);    // let Wi-Fi/idle tasks run
}

// ---------------------------------------------------------------------------
// Sensors
// ---------------------------------------------------------------------------
int readAnalogAveraged(uint8_t pin) {
  long sum = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
    sum += analogRead(pin);
    delayMicroseconds(200);
  }
  return (int)(sum / ADC_SAMPLES);
}

// Reads the soil sensor. Returns true if it looks connected.
// A sensor is "disconnected" if:
//   - the average sits at either rail (short to GND or 3V3), OR
//   - the samples are jumping around (floating pin noise).
bool readSoilSensor(
    uint8_t pin,
    int rawDry,
    int rawWet,
    int &pct,
    bool &valid,
    int &railCount
) {
  const int N = ADC_SAMPLES;
  long sum = 0;
  int mn = 4095;
  int mx = 0;

  for (int i = 0; i < N; i++) {
    int v = analogRead(pin);
    sum += v;

    if (v < mn) mn = v;
    if (v > mx) mx = v;

    delayMicroseconds(200);
  }

  int raw = (int)(sum / N);
  int spread = mx - mn;

  // A rail reading is treated as an immediate fault.
  bool railFault = (raw <= 20 || raw >= 4075);

  // A noisy/floating signal is treated as a fault after
  // consecutive bad measurements.
  bool noisyFault = (spread > SOIL_NOISE_LIMIT);

  if (railFault) {
    railCount = RAIL_COUNT_THRESHOLD;
    valid = false;
    pct = 0;
    return false;
  }

  if (noisyFault) {
    if (railCount < RAIL_COUNT_THRESHOLD) {
      railCount++;
    }

    if (railCount >= RAIL_COUNT_THRESHOLD) {
      valid = false;
      pct = 0;
      return false;
    }

    // Do not report an old reading as a new valid reading.
    valid = false;
    pct = 0;
    return false;
  }

  // A stable, in-range reading is accepted.
  railCount = 0;

  int span = rawDry - rawWet;

  if (span == 0) {
    valid = false;
    pct = 0;
    return false;
  }

  long mapped =
      ((long)(rawDry - raw) * 100L) / span;

  pct = (int)constrain(mapped, 0L, 100L);
  valid = true;

  return true;
}

void readSensors() {
  // --- DHT11 --- a single failed read is normal; only call it
  // disconnected after several in a row. Out-of-range values are rejected
  // because the server answers HTTP 400 to them (which would make the whole
  // dashboard look offline).
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  bool dhtOk = !(isnan(h) || isnan(t)) && t >= -40.0f && t <= 80.0f && h >= 0.0f && h <= 100.0f;
  if (dhtOk) {
    dhtFailCount = 0;
    temperatureC = t;
    humidityPct  = h;
    dhtConnected = true;
  } else {
    if (dhtFailCount < 1000) dhtFailCount++;
    if (dhtFailCount >= DHT_FAIL_THRESHOLD) dhtConnected = false;
  }

  // --- Two independent soil moisture sensors ---
  soil1Connected = readSoilSensor(
      SOIL1_PIN,
      SOIL1_RAW_DRY,
      SOIL1_RAW_WET,
      soil1Pct,
      soil1Valid,
      soil1RailCount
  );

  soil2Connected = readSoilSensor(
      SOIL2_PIN,
      SOIL2_RAW_DRY,
      SOIL2_RAW_WET,
      soil2Pct,
      soil2Valid,
      soil2RailCount
  );

  // Only connected sensors contribute to the control value.
  haveSoil = soil1Connected || soil2Connected;

  if (soil1Connected && soil2Connected) {
    soilAvgPct = (soil1Pct + soil2Pct + 1) / 2;

    soilCtlPct = (SOIL_CONTROL_MODE == 1)
        ? min(soil1Pct, soil2Pct)
        : soilAvgPct;

  } else if (soil1Connected) {
    soilAvgPct = soil1Pct;
    soilCtlPct = soil1Pct;

  } else if (soil2Connected) {
    soilAvgPct = soil2Pct;
    soilCtlPct = soil2Pct;

  } else {
    soilAvgPct = 0;
    soilCtlPct = 0;
  }
}

// ---------------------------------------------------------------------------
// Automation
// ---------------------------------------------------------------------------
void changeState(bool &state, bool next, unsigned long &changedAt, unsigned long now) {
  if (state != next) {
    state = next;
    changedAt = now;
  }
}

// One exhaust fan: ON at/above onC, OFF at/below offC, forced OFF without a
// temperature reading.
void runFan(bool &fanState, unsigned long &changedAt, float onC, float offC, unsigned long now) {
  if (!dhtConnected) {
    changeState(fanState, false, changedAt, now);            // fail-safe, immediate
    return;
  }
  bool want = fanState;
  if (temperatureC >= onC)       want = true;
  else if (temperatureC <= offC) want = false;
  if (want != fanState && (now - changedAt) >= (unsigned long)MIN_RELAY_HOLD_MS) {
    changeState(fanState, want, changedAt, now);
  }
}

void runAutomation(unsigned long now) {
  // ---- Exhaust fans <- temperature ----
  runFan(fan1On, fan1ChangedAt, TEMP_FAN1_ON_C, TEMP_FAN1_OFF_C, now);
  runFan(fan2On, fan2ChangedAt, TEMP_FAN2_ON_C, TEMP_FAN2_OFF_C, now);

  // ---- Pump <- soil moisture, with max-run + cooldown ----
  if (!haveSoil) {
    changeState(pumpOn, false, pumpChangedAt, now);          // fail-safe, immediate
    pumpInCooldown = false;
  } else if (pumpOn) {
    if (soilCtlPct >= SOIL_PUMP_OFF_PCT) {
      changeState(pumpOn, false, pumpChangedAt, now);        // target reached, stop at once
    } else if ((now - pumpChangedAt) >= (unsigned long)PUMP_MAX_RUN_MS) {
      changeState(pumpOn, false, pumpChangedAt, now);        // ran long enough, let water soak in
      pumpInCooldown = true;
      pumpCooldownStart = now;
      Serial.println("[Brassica] Pump max run reached, resting.");
    }
  } else {
    if (pumpInCooldown && (now - pumpCooldownStart) >= (unsigned long)PUMP_COOLDOWN_MS) {
      pumpInCooldown = false;
    }
    if (!pumpInCooldown && soilCtlPct <= SOIL_PUMP_ON_PCT &&
        (now - pumpChangedAt) >= (unsigned long)MIN_RELAY_HOLD_MS) {
      changeState(pumpOn, true, pumpChangedAt, now);
    }
  }

  writeAllRelays();
}

// ---------------------------------------------------------------------------
// Reporting to the dashboard
// ---------------------------------------------------------------------------
// Builds ONE valid JSON object. (v5.2 closed the object and then appended the
// Wi-Fi list after it, which the server rejected with HTTP 400 every time.)
String buildPayload(bool wantCtl, bool wantLog) {
  String p;
  p.reserve(1800);
  p = "{";
  if (dhtConnected) {
    p += "\"temperature\":" + String(temperatureC, 1) + ",";
    p += "\"humidity\":" + String(humidityPct, 1) + ",";
  }
  if (soil1Connected) p += "\"soil_1\":" + String(soil1Pct) + ",";
  if (soil2Connected) p += "\"soil_2\":" + String(soil2Pct) + ",";
  if (haveSoil)       p += "\"soil_moisture\":" + String(soilAvgPct) + ",";
  p += String("\"dht_connected\":")   + (dhtConnected   ? "true" : "false") + ",";
  p += String("\"soil1_connected\":") + (soil1Connected ? "true" : "false") + ",";
  p += String("\"soil2_connected\":") + (soil2Connected ? "true" : "false") + ",";
  p += String("\"fan\":\"")        + (fan1On  ? "ON" : "OFF") + "\",";
  p += String("\"fan2\":\"")       + (fan2On  ? "ON" : "OFF") + "\",";
  p += String("\"water_pump\":\"") + (pumpOn  ? "ON" : "OFF") + "\",";
  if (WiFi.status() == WL_CONNECTED) {
    p += "\"wifi_ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
    p += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    p += "\"rssi\":" + String((int)WiFi.RSSI()) + ",";
  }
  if (networksFresh) {
    p += "\"wifi_networks\":" + networksJson + ",";
  }
  if (ackId.length() > 0) {
    p += "\"wifi_ack\":{\"id\":\"" + jsonEscape(ackId) + "\",\"status\":\"" + ackStatus +
         "\",\"ssid\":\"" + jsonEscape(ackSsid) + "\",\"reason\":\"" + jsonEscape(ackReason) + "\"},";
  }
  p += String("\"ctl\":") + (wantCtl ? "true" : "false") + ",";
  p += String("\"log\":") + (wantLog ? "true" : "false") + ",";
  p += "\"fw\":\"" FIRMWARE_VERSION "\"}";
  return p;
}

// Reads the server's reply: an optional Wi-Fi change request and scan request.
void handleReply(const String& body) {
  if (jsonBoolTrue(body, "scan")) scanRequested = true;

  int w = body.indexOf("\"wifi_config\"");
  if (w < 0) return;
  String section = body.substring(w);
  String id   = jsonStringValue(section, "id");
  String ssid = hexDecode(jsonStringValue(section, "ssid_hex"));
  String pass = hexDecode(jsonStringValue(section, "pass_hex"));

  bool valid = id.length() > 0 && id.length() <= 64 &&
               ssid.length() >= 1 && ssid.length() <= 32 &&
               (pass.length() == 0 || (pass.length() >= 8 && pass.length() <= 63));
  if (!valid) {
    Serial.println("[Brassica] Ignoring a malformed Wi-Fi request from the dashboard.");
    return;
  }
  if (trialActive || id == handledId) return;      // already working on / finished this one

  pendId = id;
  pendSsid = ssid;
  pendPass = pass;
  havePend = true;
}

bool postReading() {
  if (srvTls && ESP.getMaxAllocHeap() < (size_t)MIN_HEAP_FOR_TLS) {
    Serial.printf("[Brassica] Low memory (%u), skipping post.\n", (unsigned)ESP.getMaxAllocHeap());
    if (++lowHeapCount >= 10) {
      Serial.println("[Brassica] Memory stayed low, rebooting.");
      delay(100);
      ESP.restart();
    }
    return false;
  }
  lowHeapCount = 0;

  unsigned long nowMs = millis();
  bool sentNetworks = networksFresh;
  bool sentAck = (ackId.length() > 0);
  // Ask for pending dashboard commands (Wi-Fi change / rescan) only now and then,
  // and add a history row only every few minutes: this keeps the database traffic
  // low enough for a free Redis plan.
  bool sentCtl = !ctlEverSent || sentAck || trialActive || (nowMs - lastCtlOkAt >= ctlIntervalMs());
  bool sentLog = !logEverSent || (nowMs - lastLogOkAt >= (unsigned long)HISTORY_LOG_INTERVAL_MS);
  String payload = buildPayload(sentCtl, sentLog);

  String url = srvTls ? "https://" : "http://";
  url += srvHost;
  if (srvPort > 0 && srvPort != (srvTls ? 443 : 80)) url += ":" + String(srvPort);
  url += UPDATE_PATH;

  HTTPClient http;
  WiFiClientSecure secureClient;   // fresh client every post (reusing one goes stale on ESP32)
  WiFiClient plainClient;
  bool began;

  if (srvTls) {
    secureClient.setInsecure();    // no certificate validation: simplest for a hobby device
    began = http.begin(secureClient, url);
  } else {
    began = http.begin(plainClient, url);
  }

  if (!began) {
    Serial.println("[Brassica] http.begin() failed, check the server address.");
    postFailCount++;
    return false;
  }

  http.setReuse(false);
  http.setConnectTimeout(HTTP_CONNECT_TIMEOUT_MS);
  http.setTimeout(HTTP_RESPONSE_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  if (strlen(API_KEY) > 0) {
    http.addHeader("X-API-Key", API_KEY);
  }

  int status = http.POST(payload);
  bool ok = (status == 200);
  String body = "";

  if (ok) {
    body = http.getString();
    postFailCount = 0;
    lastPostOkAt = millis();
    if (sentCtl) { ctlEverSent = true; lastCtlOkAt = lastPostOkAt; }
    if (sentLog) { logEverSent = true; lastLogOkAt = lastPostOkAt; }
    Serial.println("[Brassica] POST OK (200)");
  } else if (status > 0) {
    postFailCount++;
    Serial.printf("[Brassica] POST -> HTTP %d: %s\n", status, http.getString().c_str());
    if (status == 401) Serial.println("[Brassica] 401: API_KEY in config.h does not match BRASSICA_API_KEY on the server.");
    if (status == 503) Serial.println("[Brassica] 503: the server's database is not reachable (on Vercel: is Redis connected?).");
  } else {
    postFailCount++;
    Serial.printf("[Brassica] POST failed: %s  (RSSI %d dBm, free heap %u)\n",
                  http.errorToString(status).c_str(), (int)WiFi.RSSI(), (unsigned)ESP.getFreeHeap());
  }

  http.end();
  secureClient.stop();
  plainClient.stop();

  if (ok) {
    if (sentNetworks) networksFresh = false;
    if (sentAck) {
      if (trialActive && ackStatus == "ok") commitTrial();   // the dashboard confirmed it: keep the new network
      ackId = "";
      ackStatus = "";
      ackSsid = "";
      ackReason = "";
    }
    handleReply(body);
    if (havePend) {
      havePend = false;
      startTrial();
    }
  }
  return ok;
}

// ---------------------------------------------------------------------------
void printStatus() {
  Serial.println("-------------------------------------------");
  if (dhtConnected) Serial.printf("Temp: %.1fC  Hum: %.0f%%\n", temperatureC, humidityPct);
  else              Serial.println("Temp: OFFLINE  Hum: OFFLINE");

  if (soil1Connected) Serial.printf("Soil 1: %d%%", soil1Pct); else Serial.print("Soil 1: OFFLINE");
  if (soil2Connected) Serial.printf("   Soil 2: %d%%\n", soil2Pct); else Serial.println("   Soil 2: OFFLINE");

  Serial.printf("Pump: %s  Fan 1: %s  Fan 2: %s%s\n",
                pumpOn ? "ON" : "off", fan1On ? "ON" : "off", fan2On ? "ON" : "off",
                pumpInCooldown ? "  (pump resting)" : "");
  if (WiFi.status() == WL_CONNECTED) Serial.printf("WiFi: %s  RSSI %d dBm\n", WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  else                               Serial.println("WiFi: disconnected");
}
