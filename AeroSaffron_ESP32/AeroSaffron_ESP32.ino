/*
 * ╔══════════════════════════════════════════════════════════════╗
 *  AeroSaffron ESP32  –  v3.0  (Modern / Working)
 *  Firebase_ESP_Client  ≥ 4.x  |  database_url style
 * ══════════════════════════════════════════════════════════════
 *  GPIO MAP
 *    DHT22 data     →  GPIO  4
 *    LDR (ADC)      →  GPIO 35
 *    Relay PUMP     →  GPIO 26
 *    Relay FAN      →  GPIO 27
 *    Relay LIGHT    →  GPIO 14
 *    Relay PELTIER  →  GPIO 12
 *    OLED SDA       →  GPIO 21
 *    OLED SCL       →  GPIO 22
 *
 *  FIREBASE PATHS  (exact match with app.js)
 *    aerosaffron/sensors        temperature, humidity, lux, light
 *    aerosaffron/controls       pump, fan, light, peltier, _refresh
 *    aerosaffron/settings       controlMode + all thresholds
 *    aerosaffron/device         online, lastSeen, ip
 *    aerosaffron/alerts/msg     status string
 *
 *  LIBRARY MANAGER  (install all five)
 *    Firebase ESP Client  by Mobizt
 *    DHT sensor library   by Adafruit
 *    Adafruit Unified Sensor
 *    Adafruit SSD1306
 *    Adafruit GFX Library
 * ╚══════════════════════════════════════════════════════════════╝
 */

// ════════════════════════════════════════════════════════════════
//  ①  CHANGE THESE FOUR LINES BEFORE UPLOADING
// ════════════════════════════════════════════════════════════════
#define WIFI_SSID      "Air_Zerotouch_5G"
#define WIFI_PASS      "Airtel5g@2026"

// Full URL – copy from Firebase Console → Realtime Database
#define DB_URL         "https://aeroponics-53851-default-rtdb.asia-southeast1.firebasedatabase.app"

// Project Settings → Service Accounts → Database secrets (legacy secret)
#define DB_SECRET      "zv7ZfPaC7IH0U7XeHYn2zEDVzfoMPSmKrI0w4dhc"

// true  = most common blue relay boards (signal LOW = relay ON)
// false = active-HIGH relay boards
#define RELAY_ACTIVE_LOW  true
// ════════════════════════════════════════════════════════════════

// ── INCLUDES ────────────────────────────────────────────────────
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

#include <Firebase_ESP_Client.h>
// These two headers live inside the library's src/addons folder
#include <addons/TokenHelper.h>
#include <addons/RTDBHelper.h>

#include <DHT.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ── PINS ────────────────────────────────────────────────────────
#define PIN_DHT      4
#define PIN_LDR     35
#define PIN_PUMP    26
#define PIN_FAN     27
#define PIN_LIGHT   14
#define PIN_PELTIER 12

// ── OLED ────────────────────────────────────────────────────────
#define OLED_W  128
#define OLED_H   64
Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);
bool oledOk = false;

// ── DHT ─────────────────────────────────────────────────────────
DHT dht(PIN_DHT, DHT22);

// ── FIREBASE ────────────────────────────────────────────────────
FirebaseData   fbSet;   // set / push operations
FirebaseData   fbGet;   // get / read operations
FirebaseAuth   fbAuth;
FirebaseConfig fbCfg;

// ── TIMING ──────────────────────────────────────────────────────
#define SENSOR_MS       5000    // push sensor data every 5 s
#define POLL_CTRL_MS    2000    // read controls every 2 s
#define POLL_SET_MS    30000    // read settings every 30 s
#define HEARTBEAT_MS   10000    // push heartbeat every 10 s
#define OLED_MS         2000    // rotate OLED page every 2 s

unsigned long tSensor   = 0;
unsigned long tCtrl     = 0;
unsigned long tSettings = 0;
unsigned long tHeart    = 0;
unsigned long tOled     = 0;
uint8_t       oledPage  = 0;

// ── LIVE STATE ──────────────────────────────────────────────────
float  sTemp   = 25.0;
float  sHum    = 60.0;
int    sLux    = 200;

bool   cPump    = false;
bool   cFan     = false;
bool   cLight   = false;
bool   cPeltier = false;

// Settings (loaded from Firebase, defaults match your JSON schema)
String ctrlMode    = "auto";
float  fanMin      = 24.0,  fanMax      = 30.0;
float  peltierMin  = 18.0,  peltierMax  = 26.0;
int    lightMin    = 120,   lightMax    = 350;
int    mistDurSec  = 30;    // mist ON duration  (seconds)
int    mistIntMin  = 15;    // mist OFF interval (minutes)
String ledStart    = "06:00";
String ledEnd      = "22:00";

// Mist pump cycle
unsigned long mistOnAt   = 0;
unsigned long mistOffAt  = 0;
bool          mistActive = false;

// _refresh tracking
long lastRefresh = 0;

// ═══════════════════════════════════════════════════════════════
//  RELAY HELPERS
// ═══════════════════════════════════════════════════════════════

inline void relayOn(uint8_t pin)  { digitalWrite(pin, RELAY_ACTIVE_LOW ? LOW  : HIGH); }
inline void relayOff(uint8_t pin) { digitalWrite(pin, RELAY_ACTIVE_LOW ? HIGH : LOW);  }

void applyRelays() {
  cPump    ? relayOn(PIN_PUMP)    : relayOff(PIN_PUMP);
  cFan     ? relayOn(PIN_FAN)     : relayOff(PIN_FAN);
  cLight   ? relayOn(PIN_LIGHT)   : relayOff(PIN_LIGHT);
  cPeltier ? relayOn(PIN_PELTIER) : relayOff(PIN_PELTIER);

  Serial.printf("[Relay] pump=%d  fan=%d  light=%d  peltier=%d\n",
                cPump, cFan, cLight, cPeltier);
}

// ═══════════════════════════════════════════════════════════════
//  FIREBASE PUSH HELPERS  (individual fields – most reliable)
// ═══════════════════════════════════════════════════════════════

bool fbSetFloat(const char* path, float v) {
  return Firebase.RTDB.setFloat(&fbSet, path, v);
}
bool fbSetInt(const char* path, int v) {
  return Firebase.RTDB.setInt(&fbSet, path, v);
}
bool fbSetStr(const char* path, const String& v) {
  return Firebase.RTDB.setString(&fbSet, path, v);
}
bool fbSetBool(const char* path, bool v) {
  return Firebase.RTDB.setBool(&fbSet, path, v);
}

// ═══════════════════════════════════════════════════════════════
//  PUSH SENSORS  →  aerosaffron/sensors/*
// ═══════════════════════════════════════════════════════════════

void pushSensors() {
  bool ok = true;
  ok &= fbSetFloat("aerosaffron/sensors/temperature", sTemp);
  ok &= fbSetFloat("aerosaffron/sensors/humidity",    sHum);
  ok &= fbSetInt  ("aerosaffron/sensors/lux",         sLux);
  ok &= fbSetInt  ("aerosaffron/sensors/light",       sLux);  // legacy

  if (ok) Serial.printf("[FB] Sensors pushed  T=%.1f  H=%.1f  L=%d\n",
                         sTemp, sHum, sLux);
  else    Serial.printf("[FB] Sensor push failed: %s\n", fbSet.errorReason().c_str());
}

// ═══════════════════════════════════════════════════════════════
//  PUSH CONTROLS  →  aerosaffron/controls/*
//  (called by auto mode so dashboard reflects real state)
// ═══════════════════════════════════════════════════════════════

void pushControls() {
  fbSetInt("aerosaffron/controls/pump",    cPump    ? 1 : 0);
  fbSetInt("aerosaffron/controls/fan",     cFan     ? 1 : 0);
  fbSetInt("aerosaffron/controls/light",   cLight   ? 1 : 0);
  fbSetInt("aerosaffron/controls/peltier", cPeltier ? 1 : 0);
}

// ═══════════════════════════════════════════════════════════════
//  PUSH ALERT  →  aerosaffron/alerts/msg
// ═══════════════════════════════════════════════════════════════

void pushAlert(const String& msg) {
  fbSetStr("aerosaffron/alerts/msg", msg);
  Serial.printf("[Alert] %s\n", msg.c_str());
}

// ═══════════════════════════════════════════════════════════════
//  HEARTBEAT  →  aerosaffron/device/*
// ═══════════════════════════════════════════════════════════════

void pushHeartbeat() {
  time_t now = time(nullptr);
  unsigned long long ts = (now > 100000)
    ? ((unsigned long long)now * 1000ULL)
    : (unsigned long long)millis();
  char tsBuf[24];
  snprintf(tsBuf, sizeof(tsBuf), "%llu", ts);

  fbSetBool("aerosaffron/device/online", true);
  fbSetStr ("aerosaffron/device/ip",     WiFi.localIP().toString());
  // lastSeen must be a number – app.js: (Date.now() - lastSeen) < 20000
  fbSetStr ("aerosaffron/device/lastSeen", String(tsBuf));

  Serial.printf("[Heart] online=true  ts=%s  ip=%s\n",
                tsBuf, WiFi.localIP().toString().c_str());
}

// ═══════════════════════════════════════════════════════════════
//  READ CONTROLS  ←  aerosaffron/controls/*
//  Called every POLL_CTRL_MS. In manual mode this is how the
//  ESP32 sees toggle changes made on the dashboard.
// ═══════════════════════════════════════════════════════════════

void pollControls() {
  if (!Firebase.RTDB.getJSON(&fbGet, "aerosaffron/controls")) {
    Serial.printf("[FB] controls read failed: %s\n", fbGet.errorReason().c_str());
    return;
  }

  FirebaseJson    *j = fbGet.jsonObjectPtr();
  FirebaseJsonData d;

  auto getInt = [&](const char* key) -> int {
    return j->get(d, key) ? d.intValue : 0;
  };

  cPump    = getInt("pump")    == 1;
  cFan     = getInt("fan")     == 1;
  cLight   = getInt("light")   == 1;
  cPeltier = getInt("peltier") == 1;

  // Detect _refresh (dashboard switches Auto<->Manual)
  long newRefresh = getInt("_refresh");
  if (newRefresh != lastRefresh && newRefresh != 0) {
    lastRefresh = newRefresh;
    Serial.println("[FB] _refresh received - reloading settings now");
    tSettings = 0;   // force immediate settings re-read
  }

  // In manual mode - apply relays immediately after reading
  if (ctrlMode == "manual") {
    applyRelays();
  }
}

// ═══════════════════════════════════════════════════════════════
//  READ SETTINGS  ←  aerosaffron/settings/*
// ═══════════════════════════════════════════════════════════════

void pollSettings() {
  if (!Firebase.RTDB.getJSON(&fbGet, "aerosaffron/settings")) {
    Serial.printf("[FB] settings read failed: %s\n", fbGet.errorReason().c_str());
    return;
  }

  FirebaseJson    *j = fbGet.jsonObjectPtr();
  FirebaseJsonData d;

  auto getStr   = [&](const char* k) -> String { return j->get(d,k) ? d.stringValue : ""; };
  auto getFloat = [&](const char* k, float def) -> float {
    return j->get(d,k) ? (float)d.doubleValue : def;
  };
  auto getInt2  = [&](const char* k, int def) -> int {
    return j->get(d,k) ? d.intValue : def;
  };

  String newMode = getStr("controlMode");
  if (newMode.length()) {
    bool modeChanged = (newMode != ctrlMode);
    ctrlMode = newMode;
    if (modeChanged)
      Serial.printf("[Settings] controlMode changed -> %s\n", ctrlMode.c_str());
  }

  fanMin      = getFloat("fanMin",      fanMin);
  fanMax      = getFloat("fanMax",      fanMax);
  peltierMin  = getFloat("peltierMin",  peltierMin);
  peltierMax  = getFloat("peltierMax",  peltierMax);
  lightMin    = getInt2 ("lightMin",    lightMin);
  lightMax    = getInt2 ("lightMax",    lightMax);
  mistDurSec  = getInt2 ("mistDuration",mistDurSec);
  mistIntMin  = getInt2 ("mistInterval",mistIntMin);

  String ls = getStr("ledStart");  if (ls.length())  ledStart = ls;
  String le = getStr("ledEnd");    if (le.length())  ledEnd   = le;

  Serial.printf("[Settings] mode=%s fan[%.0f-%.0f] peltier[%.0f-%.0f] lux[%d-%d]\n",
    ctrlMode.c_str(), fanMin, fanMax, peltierMin, peltierMax, lightMin, lightMax);
}

// ═══════════════════════════════════════════════════════════════
//  READ SENSORS from DHT22 + LDR
// ═══════════════════════════════════════════════════════════════

void readSensors() {
  float t = dht.readTemperature();
  float h = dht.readHumidity();

  if (!isnan(t) && t > -40 && t < 85)  sTemp = t;
  else Serial.println("[DHT] temp read error");

  if (!isnan(h) && h >= 0 && h <= 100) sHum  = h;
  else Serial.println("[DHT] hum read error");

  // LDR on GPIO35 (ADC1 channel, 12-bit)
  // Higher raw = more light hitting LDR = lower resistance = higher lux
  // Calibrate: cover LDR, note raw; expose to full light, note raw; adjust map()
  int raw = analogRead(PIN_LDR);
  sLux = map(raw, 0, 4095, 0, 1000);   // 0 = dark, 1000 = very bright
  sLux = constrain(sLux, 0, 1000);

  Serial.printf("[Sensor] T=%.1f C  H=%.1f%%  Lux=%d  (LDR raw=%d)\n",
                sTemp, sHum, sLux, raw);
}

// ═══════════════════════════════════════════════════════════════
//  LED SCHEDULE HELPER
// ═══════════════════════════════════════════════════════════════

int hhmm2min(const String &t) {
  return t.substring(0,2).toInt() * 60 + t.substring(3,5).toInt();
}

bool inLedSchedule() {
  time_t now = time(nullptr);
  if (now < 100000) return true;   // NTP not ready - assume schedule active
  struct tm *ti = localtime(&now);
  int cur   = ti->tm_hour * 60 + ti->tm_min;
  int start = hhmm2min(ledStart);
  int end   = hhmm2min(ledEnd);
  return (start <= end) ? (cur >= start && cur < end)
                        : (cur >= start || cur < end);
}

// ═══════════════════════════════════════════════════════════════
//  AUTO MODE LOGIC
// ═══════════════════════════════════════════════════════════════

void runAutoMode() {
  bool changed = false;

  // ── FAN  (hysteresis) ───────────────────────────────────────
  if (!cFan && sTemp >= fanMax) {
    cFan = true; changed = true;
    pushAlert("Auto: Fan ON - Temp " + String(sTemp, 1) + "C");
  } else if (cFan && sTemp <= fanMin) {
    cFan = false; changed = true;
    pushAlert("System Normal");
  }

  // ── PELTIER  (hysteresis) ───────────────────────────────────
  if (!cPeltier && sTemp >= peltierMax) {
    cPeltier = true; changed = true;
    pushAlert("Auto: Peltier ON - Temp " + String(sTemp, 1) + "C");
  } else if (cPeltier && sTemp <= peltierMin) {
    cPeltier = false; changed = true;
    pushAlert("System Normal");
  }

  // ── GROW LIGHT  (lux thresholds + schedule) ─────────────────
  bool schedOk = inLedSchedule();
  if (!cLight && schedOk && sLux <= lightMin) {
    cLight = true; changed = true;
    pushAlert("Auto: Light ON - Lux " + String(sLux));
  } else if (cLight && (!schedOk || sLux >= lightMax)) {
    cLight = false; changed = true;
    pushAlert("System Normal");
  }

  // ── MIST PUMP  (interval / duration cycle) ──────────────────
  unsigned long nowMs    = millis();
  unsigned long onDurMs  = (unsigned long)mistDurSec * 1000UL;
  unsigned long offDurMs = (unsigned long)mistIntMin * 60000UL;

  if (!mistActive) {
    if (nowMs - mistOffAt >= offDurMs) {
      cPump = true;  mistActive = true;  mistOnAt = nowMs;
      changed = true;
      pushAlert("Auto: Mist cycle started");
    }
  } else {
    if (nowMs - mistOnAt >= onDurMs) {
      cPump = false;  mistActive = false;  mistOffAt = nowMs;
      changed = true;
      pushAlert("System Normal");
    }
  }

  // Push updated control states to Firebase so dashboard shows them
  if (changed) {
    pushControls();
  }
  applyRelays();
}

// ═══════════════════════════════════════════════════════════════
//  OLED  (4 rotating pages, 2 s each)
// ═══════════════════════════════════════════════════════════════

void updateOled() {
  if (!oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);

  switch (oledPage % 4) {

    case 0:  // Temperature & Humidity
      oled.setTextSize(1); oled.setCursor(22, 0);
      oled.print("AeroSaffron");
      oled.drawLine(0, 9, 127, 9, SSD1306_WHITE);
      oled.setTextSize(2); oled.setCursor(0, 14);
      oled.printf("T %.1f C", sTemp);
      oled.setTextSize(2); oled.setCursor(0, 36);
      oled.printf("H %.1f %%", sHum);
      break;

    case 1:  // Lux
      oled.setTextSize(1); oled.setCursor(30, 0);
      oled.print("Light/Lux");
      oled.drawLine(0, 9, 127, 9, SSD1306_WHITE);
      oled.setTextSize(2); oled.setCursor(0, 14);
      oled.printf("%d lx", sLux);
      oled.setTextSize(1); oled.setCursor(0, 44);
      oled.printf("Min:%d  Max:%d", lightMin, lightMax);
      oled.setCursor(0, 54);
      oled.printf("Sched: %s", inLedSchedule() ? "ON" : "OFF");
      break;

    case 2:  // Device states
      oled.setTextSize(1); oled.setCursor(16, 0);
      oled.print("Device Status");
      oled.drawLine(0, 9, 127, 9, SSD1306_WHITE);
      oled.setCursor(0, 13);
      oled.printf("Pump   : %s\n", cPump    ? "ON " : "OFF");
      oled.printf("Fan    : %s\n", cFan     ? "ON " : "OFF");
      oled.printf("Light  : %s\n", cLight   ? "ON " : "OFF");
      oled.printf("Peltier: %s\n", cPeltier ? "ON " : "OFF");
      break;

    case 3:  // WiFi / Firebase / mode
      oled.setTextSize(1); oled.setCursor(18, 0);
      oled.print("System Status");
      oled.drawLine(0, 9, 127, 9, SSD1306_WHITE);
      oled.setCursor(0, 13);
      oled.printf("WiFi: %s\n",  WiFi.status()==WL_CONNECTED ? "Connected" : "!! DOWN");
      oled.printf("FB  : %s\n",  Firebase.ready() ? "Ready" : "Wait..");
      oled.printf("Mode: %s\n",  ctrlMode.c_str());
      oled.printf("IP  : %s\n",  WiFi.localIP().toString().c_str());
      break;
  }
  oled.display();
}

// ═══════════════════════════════════════════════════════════════
//  WIFI CONNECT
// ═══════════════════════════════════════════════════════════════

void connectWiFi() {
  Serial.printf("\n[WiFi] Connecting to '%s'", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint8_t tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(500);
    Serial.print(".");
    tries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Connected   IP: %s\n",
                  WiFi.localIP().toString().c_str());
    // NTP - IST = UTC+5:30
    configTime(5*3600 + 1800, 0, "pool.ntp.org", "time.google.com");
    Serial.println("[NTP]  Syncing...");
    delay(2000);
  } else {
    Serial.println("\n[WiFi] FAILED - restarting in 5 s");
    delay(5000);
    ESP.restart();
  }
}

// ═══════════════════════════════════════════════════════════════
//  FIREBASE INIT
// ═══════════════════════════════════════════════════════════════

void initFirebase() {
  fbCfg.database_url               = DB_URL;
  fbCfg.signer.tokens.legacy_token = DB_SECRET;
  fbCfg.token_status_callback      = tokenStatusCallback;  // from TokenHelper.h

  // Larger buffer for JSON payloads
  fbSet.setResponseSize(2048);
  fbGet.setResponseSize(2048);

  Firebase.begin(&fbCfg, &fbAuth);
  Firebase.reconnectWiFi(true);

  // Wait until Firebase is ready (token exchange)
  Serial.print("[FB]   Waiting for Firebase");
  uint8_t tries = 0;
  while (!Firebase.ready() && tries < 30) {
    Serial.print(".");
    delay(500);
    tries++;
  }
  Serial.println(Firebase.ready() ? "\n[FB]   Ready" : "\n[FB]   Timeout - continuing anyway");
}

// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== AeroSaffron ESP32  v3.0 ===");

  // ── Relays - all OFF at boot ─────────────────────────────────
  uint8_t relPins[] = { PIN_PUMP, PIN_FAN, PIN_LIGHT, PIN_PELTIER };
  for (uint8_t i = 0; i < 4; i++) {
    pinMode(relPins[i], OUTPUT);
    relayOff(relPins[i]);
  }

  // ── ADC (LDR) ────────────────────────────────────────────────
  analogReadResolution(12);
  pinMode(PIN_LDR, INPUT);

  // ── DHT22 ────────────────────────────────────────────────────
  dht.begin();
  delay(2000);  // DHT22 needs ~2 s after power-on

  // ── OLED ─────────────────────────────────────────────────────
  Wire.begin(21, 22);
  if (oled.begin(SSD1306_SWITCHCAPVCC, 0x3C) ||
      oled.begin(SSD1306_SWITCHCAPVCC, 0x3D)) {
    oledOk = true;
    oled.clearDisplay();
    oled.setTextSize(1);
    oled.setTextColor(SSD1306_WHITE);
    oled.setCursor(20, 20); oled.println("AeroSaffron");
    oled.setCursor(18, 36); oled.println("Starting...");
    oled.display();
    Serial.println("[OLED] Found");
  } else {
    Serial.println("[OLED] Not found - continuing without display");
  }

  // ── WiFi + NTP + Firebase ────────────────────────────────────
  connectWiFi();
  initFirebase();

  // ── Load initial settings + controls from Firebase ───────────
  Serial.println("[Init] Loading settings from Firebase...");
  pollSettings();
  pollControls();

  // ── Mist pump timer: first cycle starts after one full interval
  mistOffAt = millis();

  // ── Mark device online ───────────────────────────────────────
  pushHeartbeat();
  pushAlert("System Normal");

  Serial.println("[Init] Setup complete - entering main loop\n");
}

// ═══════════════════════════════════════════════════════════════
//  LOOP
// ═══════════════════════════════════════════════════════════════

void loop() {
  unsigned long now = millis();

  // ── WiFi watchdog ────────────────────────────────────────────
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Lost - reconnecting...");
    WiFi.reconnect();
    delay(5000);
    return;
  }

  // ── Poll controls from Firebase (manual mode + _refresh) ─────
  if (now - tCtrl >= POLL_CTRL_MS) {
    tCtrl = now;
    pollControls();
  }

  // ── Poll settings from Firebase ──────────────────────────────
  if (now - tSettings >= POLL_SET_MS) {
    tSettings = now;
    pollSettings();
  }

  // ── Read sensors, push to Firebase, run mode logic ───────────
  if (now - tSensor >= SENSOR_MS) {
    tSensor = now;
    readSensors();
    pushSensors();

    if (ctrlMode == "auto") {
      runAutoMode();
    }
    // In manual mode, relays are applied inside pollControls()
  }

  // ── Heartbeat ─────────────────────────────────────────────────
  if (now - tHeart >= HEARTBEAT_MS) {
    tHeart = now;
    pushHeartbeat();
  }

  // ── OLED rotate ───────────────────────────────────────────────
  if (now - tOled >= OLED_MS) {
    tOled = now;
    updateOled();
    oledPage++;
  }

  delay(10);  // yield to background WiFi tasks
}

/*
 * ═══════════════════════════════════════════════════════════════
 *  QUICK-START / TROUBLESHOOTING
 * ═══════════════════════════════════════════════════════════════
 *
 *  1. Fill in WIFI_SSID, WIFI_PASS, DB_URL, DB_SECRET at the top.
 *
 *  2. DB_URL format - no trailing slash:
 *       https://aeroponics-53851-default-rtdb.asia-southeast1.firebasedatabase.app
 *
 *  3. DB_SECRET - get it from:
 *       Firebase Console -> Project Settings ->
 *       Service Accounts -> Database secrets -> Show (legacy secret)
 *
 *  4. Board: "ESP32 Dev Module"  |  Upload speed: 115200
 *
 *  5. Open Serial Monitor @ 115200. You should see:
 *       [WiFi] Connected  IP: 192.168.x.x
 *       [FB]   Ready
 *       [Sensor] T=25.0 C  H=60.1%  Lux=210
 *       [FB] Sensors pushed  T=25.0  H=60.1  L=210
 *       [Heart] online=true
 *     If you see "sensor push failed" -> check DB_SECRET.
 *
 *  6. LDR calibration:
 *       Cover LDR completely -> read "LDR raw=NNN" -> dark value.
 *       Point at bright light -> read raw again -> bright value.
 *       Update map(raw, DARK, BRIGHT, 0, 1000) in readSensors().
 *
 *  7. Dashboard Manual toggle test:
 *       - Open dashboard -> switch to Manual mode.
 *       - Toggle Pump ON.
 *       - Within ~2 s the ESP32 polls controls, relay clicks.
 *       - Serial Monitor shows: [Relay] pump=1  fan=0  light=0  peltier=0
 *
 *  8. RELAY_ACTIVE_LOW:
 *       Most blue 4-channel relay boards = true (LOW = relay ON).
 *       If relays are inverted, change to false.
 *
 *  9. Firebase rules - during development set to allow all reads/writes:
 *       { "rules": { ".read": true, ".write": true } }
 *     Tighten rules before going to production.
 * ═══════════════════════════════════════════════════════════════
 */
