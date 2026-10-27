/*
  Brassica_Diagnostics.ino  (v6.0)
  Stand-alone wiring + calibration helper. NOT the production firmware.

  Run this FIRST. It needs no WiFi. It lets you:
    1. Confirm DHT11, soil sensor and light sensor are wired and reading.
    2. Test each relay channel one at a time (and confirm active-LOW vs HIGH).
    3. Read the raw numbers for calibration (SOIL_RAW_DRY/WET, LDR_RAW_DARK/BRIGHT).

  Serial Monitor: 115200 baud, line ending "Newline".
  COMMANDS:  p1/p0 = pump (IN1)   l1/l0 = warm light (IN2)
             f1/f0 = fan (IN3)    s1/s0 = spare (IN4)    a0 = ALL OFF
  Readings print every 2.5 seconds.

  TIP: do the first relay test with NOTHING connected to the relay outputs.
  You should hear/see the relay click and its LED light when you send e.g. "p1".
  If "p1" turns the relay OFF and "p0" turns it ON, set RELAY_ACTIVE_LOW to
  false in BOTH this file and config.h.

  Pins here must match Brassica_ESP32/config.h.
*/

#include <DHT.h>

#define DHT_PIN          26
#define DHT_TYPE         DHT11
#define SOIL_PIN         34
#define LDR_PIN          35
#define RELAY_PUMP_PIN   25
#define RELAY_LIGHT_PIN  33
#define RELAY_FAN_PIN    27
#define RELAY_SPARE_PIN  32
#define RELAY_ACTIVE_LOW true

DHT dht(DHT_PIN, DHT_TYPE);
unsigned long lastRead = 0;

void setRelay(uint8_t pin, bool on) {
  bool level = RELAY_ACTIVE_LOW ? !on : on;
  digitalWrite(pin, level ? HIGH : LOW);
}

void allOff() {
  setRelay(RELAY_PUMP_PIN, false);
  setRelay(RELAY_LIGHT_PIN, false);
  setRelay(RELAY_FAN_PIN, false);
  setRelay(RELAY_SPARE_PIN, false);
}

int readAvg(uint8_t pin) {
  long sum = 0;
  for (int i = 0; i < 16; i++) { sum += analogRead(pin); delayMicroseconds(200); }
  return (int)(sum / 16);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(RELAY_PUMP_PIN, OUTPUT);
  pinMode(RELAY_LIGHT_PIN, OUTPUT);
  pinMode(RELAY_FAN_PIN, OUTPUT);
  pinMode(RELAY_SPARE_PIN, OUTPUT);
  allOff();

  dht.begin();

  Serial.println("\n=== Brassica Diagnostics ===");
  Serial.println("Commands: p1/p0 pump, l1/l0 warm light, f1/f0 fan, s1/s0 spare, a0 all off");
  Serial.println("Readings print every 2.5 seconds.\n");
}

void loop() {
  handleSerialCommands();
  if (millis() - lastRead >= 2500) {
    lastRead = millis();
    printReadings();
  }
}

void handleSerialCommands() {
  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if      (cmd == "p1") { setRelay(RELAY_PUMP_PIN, true);   Serial.println(">> Pump relay (IN1) -> ON"); }
  else if (cmd == "p0") { setRelay(RELAY_PUMP_PIN, false);  Serial.println(">> Pump relay (IN1) -> OFF"); }
  else if (cmd == "l1") { setRelay(RELAY_LIGHT_PIN, true);  Serial.println(">> Warm light relay (IN2) -> ON"); }
  else if (cmd == "l0") { setRelay(RELAY_LIGHT_PIN, false); Serial.println(">> Warm light relay (IN2) -> OFF"); }
  else if (cmd == "f1") { setRelay(RELAY_FAN_PIN, true);    Serial.println(">> Fan relay (IN3) -> ON"); }
  else if (cmd == "f0") { setRelay(RELAY_FAN_PIN, false);   Serial.println(">> Fan relay (IN3) -> OFF"); }
  else if (cmd == "s1") { setRelay(RELAY_SPARE_PIN, true);  Serial.println(">> Spare relay (IN4) -> ON"); }
  else if (cmd == "s0") { setRelay(RELAY_SPARE_PIN, false); Serial.println(">> Spare relay (IN4) -> OFF"); }
  else if (cmd == "a0") { allOff();                         Serial.println(">> ALL relays OFF"); }
  else if (cmd.length() > 0) { Serial.println("Unknown command. Use p/l/f/s with 1 or 0, or a0."); }
}

void printReadings() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  int soilRaw = readAvg(SOIL_PIN);
  int ldrRaw  = readAvg(LDR_PIN);

  Serial.println("-------------------------------------------");
  if (isnan(h) || isnan(t)) {
    Serial.println("DHT11:  READ FAILED. Check VCC (3.3V), GND, data wire on the DHT pin.");
  } else {
    Serial.printf("DHT11:  Temp=%.1fC  Humidity=%.0f%%\n", t, h);
  }
  Serial.printf("Soil (raw 0-4095): %d   <- note value in DRY air and in WATER\n", soilRaw);
  Serial.printf("Light(raw 0-4095): %d   <- note value COVERED/dark and in BRIGHT light\n", ldrRaw);
}
