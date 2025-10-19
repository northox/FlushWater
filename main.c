#include <ESP8266WiFi.h>
#include <ezTime.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>

#define ADC_PIN A0
#define PUMP_RELAY_PIN 5            // Output that controls inhibit/allow
#define EXT_LED_PIN 4               // D2
#define INHIBIT_ACTIVE_LEVEL HIGH   // Set to LOW if your relay logic is inverted
#define SLEEP 10000
#define MORNING 5
#define NIGHT 22
#define ROD_LENGTH 42
#define SOME_THRESHOLD 0.5          // kept but no longer used for decision
#define MAX_BACKOFF 40000UL

const char* ssid = "ssid";
const char* password = "pass";
const char* mqttServer = "192.168.1.1";
const int   mqttPort = 1883;
const char* mqttUser = "user";
const char* mqttPassword = "pass";
const char* ntpServer = "pool.ntp.org";
const long  timeZoneOffset_sec = -5 * 60 * 60;
const int   daylightOffset_sec = 3600;
const int   waterLevelThreshold = 5;     // used for night eligibility
const int   criticalWaterLevel = 32;     // used for day threshold 
const int   minimumWaterLevel = 0;
const unsigned long pumpOperationTimeout = 5UL * 60000UL; // max allow window
const int   expectedDrop = 2;            // 2 cm per 5 min expectation
bool pumpOperationSafe = true;
unsigned long lastLevelPubMs = 0;
int lastLevelPubCm = INT_MIN;
const unsigned long LEVEL_PUB_PERIOD_MS = 1200000; // 30 s
const int LEVEL_PUB_DELTA_CM = 2;               // publish on ≥2 cm change

static unsigned long lastCheck = 0;
int level = 0;
unsigned long wifiBackoff = 3000;

WiFiClient espClient;
Timezone myTZ;
PubSubClient mqttClient(espClient);

// LED heartbeat/pattern state
bool allowActive = false;             // "allowed" window, not actual motor state
unsigned long hbLastToggle = 0;
const unsigned long hbIntervalMs = 500;
unsigned long extPatternStart = 0;

// Allow/deny windows
unsigned long allowUntil = 0;         // millis timestamp; allow until this time
unsigned long noRearmUntil = 0;       // refractory window end

// Rise detection window
const unsigned long RISE_WINDOW_SEC = 300; // 5 minutes

const int lookupTable[][2] = {
  {188, 0}, {255, 5}, {350, 10}, {411, 15}, {473, 20}, {555, 25},
  {608, 30}, {658, 35}, {747, 37}, {826, 39}, {900, 40}, {903, 41}, {980, 42}
};
const int lookupTableSize = sizeof(lookupTable) / sizeof(lookupTable[0]);

struct WaterLevelReading {
  int lvl;
  time_t timestamp;   // seconds epoch
};

const int bufferSize = 24;
WaterLevelReading readings[bufferSize];
int currentReadingIndex = 0;

void setupWIFI() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.begin(ssid, password);

  const unsigned long BOOT_WIFI_DEADLINE = millis() + 20000UL; // 20 s
  while (WiFi.status() != WL_CONNECTED && (long)(millis() - BOOT_WIFI_DEADLINE) < 0) {
    yield();  // let Wi-Fi stack run
    delay(50);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi up: "); Serial.println(WiFi.localIP());
    wifiBackoff = 2000; // reset for runtime
  } else {
    Serial.println("WiFi boot connect timeout; will retry in loop.");
  }
}

void ensureWIFI() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("WIFI not connected... Retrying... ");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < wifiBackoff) {
    WiFi.reconnect();
    for (int i = 0; i < 10; i++) { yield(); delay(50); } // ~500 ms with yields
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("connected.");
    wifiBackoff = 2000;
  } else {
    Serial.println("attempt failed.");
    wifiBackoff = min(wifiBackoff * 2, MAX_BACKOFF);
  }
}

void setupMQTT() {
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  ensureMQTT();
}

void ensureMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;   // guard
  if (mqttClient.connected()) return;

  Serial.print("MQTT not connected... ");
  for (int i = 3; i > 0 && !mqttClient.connected(); i--) {
    // unique client id avoids broker session collisions on rapid reboots
    String cid = String("ESP8266Client-") + String(ESP.getChipId(), HEX);
    mqttClient.connect(cid.c_str(), mqttUser, mqttPassword);
    for (int k = 0; k < 10; k++) { mqttClient.loop(); yield(); delay(50); }
  }
  if (!mqttClient.connected()) {
    Serial.println("attempt failed.");
  } else {
    Serial.println("connected.");
    mqttClient.publish("pool/sumppump/log", "Reconnect.");
    mqttClient.subscribe("pool/sumppump/safe");
  }
}

void setupNTP() {
  if (timeStatus() != timeSet) {
    Serial.print("Setting time... ");
    for (int i = 3; i > 0 && timeStatus() != timeSet; i--) {
      updateNTP();
      delay(1000);
    }
    if (timeStatus() != timeSet) {
      Serial.println("attempt failed. Default time set to noon, January 1, 2020.");
      myTZ.setTime(12, 0, 0, 1, JANUARY, 2020);
    } else {
      Serial.println("synchronized: " + myTZ.dateTime());
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived on topic: ");
  Serial.println(topic);
  if (strcmp(topic, "pool/sumppump/safe") == 0) {
    String message = "";
    for (int i = 0; i < length; i++) message += (char)payload[i];
    pumpOperationSafe = (message != "no");
    Serial.println(pumpOperationSafe ? "Received safety status: safe to operate pump."
                                     : "Received safety status: unsafe to operate pump.");
  }
}

void maybePublishLevel() {
  if (!mqttClient.connected()) return;
  unsigned long nowMs = millis();
  bool timeElapsed = (nowMs - lastLevelPubMs) >= LEVEL_PUB_PERIOD_MS;
  bool bigDelta = (lastLevelPubCm == INT_MIN) || (abs(level - lastLevelPubCm) >= LEVEL_PUB_DELTA_CM);
  if (timeElapsed || bigDelta) {
    if (mqttClient.publish("pool/sumppump/level", String(level).c_str())) {
      lastLevelPubMs = nowMs;
      lastLevelPubCm = level;
    }
  }
}

void setInhibit(bool inhibit) {
  digitalWrite(PUMP_RELAY_PIN, inhibit ? INHIBIT_ACTIVE_LEVEL : !INHIBIT_ACTIVE_LEVEL);
}

bool isInhibited() {
  return digitalRead(PUMP_RELAY_PIN) == INHIBIT_ACTIVE_LEVEL;
}

void setup() {
  Serial.begin(9600);
  Serial.println("Booting.");
  ESP.wdtDisable();
  ESP.wdtEnable(60000);

  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(EXT_LED_PIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  digitalWrite(EXT_LED_PIN, LOW);

  pinMode(PUMP_RELAY_PIN, OUTPUT);
  setInhibit(true); // safe default on boot

  setupWIFI();

  myTZ.setLocation(F("America/New_York"));
  setServer(ntpServer);
  setInterval(3600);

  setupMQTT();
  extPatternStart = millis();
}

// -------- level, buffer, interpolation ----------
int interpolateWaterLevel(int raw) {
  if (raw <= lookupTable[0][0]) return lookupTable[0][1];
  if (raw >= lookupTable[lookupTableSize - 1][0]) return lookupTable[lookupTableSize - 1][1];

  for (int i = 0; i < lookupTableSize - 1; i++) {
    if (raw == lookupTable[i][0]) {
      return ROD_LENGTH - lookupTable[i][1];
    }
    if (raw > lookupTable[i][0] && raw < lookupTable[i + 1][0]) {
      float slope = (float)(lookupTable[i + 1][1] - lookupTable[i][1]) /
                    (lookupTable[i + 1][0] - lookupTable[i][0]);
      float intercept = lookupTable[i][1] - slope * lookupTable[i][0];
      return ROD_LENGTH - (slope * raw + intercept);
    }
  }
  return -1;
}

void updateBuffer(float lvl) {
  time_t now = time(nullptr);
  readings[currentReadingIndex].lvl = lvl;
  readings[currentReadingIndex].timestamp = now;
  currentReadingIndex = (currentReadingIndex + 1) % bufferSize;
}

void getWaterLevel(bool update = true) {
  level = interpolateWaterLevel(analogRead(ADC_PIN));
  delay(500); // left as-is
  if (update) updateBuffer(level);
  Serial.print("Water Level: ");
  Serial.print(level);
  Serial.println(" cm");
}

// Find a reading ~windowSec seconds ago; fallback to oldest available
bool findReadingOlderThan(unsigned long windowSec, int& outLvl, time_t& outTs) {
  time_t nowSec = time(nullptr);
  time_t target = nowSec - (time_t)windowSec;

  // Walk buffer backwards to find the latest reading older than target
  for (int i = 0; i < bufferSize; i++) {
    int idx = (currentReadingIndex - 1 - i + bufferSize) % bufferSize;
    time_t ts = readings[idx].timestamp;
    if (ts == 0) continue; // uninitialized
    if (ts <= target) {
      outLvl = readings[idx].lvl;
      outTs = ts;
      return true;
    }
  }
  // Fallback: pick the oldest initialized
  for (int i = 0; i < bufferSize; i++) {
    int idx = (currentReadingIndex - 1 - i + bufferSize) % bufferSize;
    time_t ts = readings[idx].timestamp;
    if (ts != 0) {
      outLvl = readings[idx].lvl;
      outTs = ts;
      return true;
    }
  }
  return false;
}

// cm per minute over window
float riseCmPerMin(unsigned long windowSec) {
  int oldLvl = 0; time_t oldTs = 0;
  if (!findReadingOlderThan(windowSec, oldLvl, oldTs)) return 0.0f;
  time_t nowSec = time(nullptr);
  if (nowSec <= oldTs) return 0.0f;
  float delta = (float)level - (float)oldLvl;         // positive means rising
  float minutes = (nowSec - oldTs) / 60.0f;
  return delta / minutes;
}

// -------- LED patterns (reuse allowActive) --------
void getExtLedPattern(unsigned long& periodMs, unsigned long& onMs, bool& solidOn) {
  bool wifiOK  = (WiFi.status() == WL_CONNECTED);
  bool mqttOK  = mqttClient.connected();
  bool timeOK  = (timeStatus() == timeSet);

  if (allowActive) { solidOn = true; periodMs = 0; onMs = 0; return; }           // allowed window
  if (!pumpOperationSafe) { solidOn = false; periodMs = 2000; onMs = 1000; return; }
  if (!wifiOK)           { solidOn = false; periodMs = 250;  onMs = 125;  return; }
  if (!mqttOK)           { solidOn = false; periodMs = 500;  onMs = 250;  return; }
  if (!timeOK)           { solidOn = false; periodMs = 3000; onMs = 200;  return; }
  solidOn = false; periodMs = 1000; onMs = 500; // normal
}

void driveLedsNonBlocking() {
  unsigned long nowMs = millis();
  if (nowMs - hbLastToggle >= hbIntervalMs) {
    hbLastToggle = nowMs;
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  }
  unsigned long periodMs, onMs; bool solidOn;
  getExtLedPattern(periodMs, onMs, solidOn);
  if (solidOn) { digitalWrite(EXT_LED_PIN, HIGH); extPatternStart = nowMs; return; }
  if (periodMs == 0) { digitalWrite(EXT_LED_PIN, LOW); extPatternStart = nowMs; return; }
  unsigned long phase = (nowMs - extPatternStart) % periodMs;
  digitalWrite(EXT_LED_PIN, (phase < onMs) ? HIGH : LOW);
}

// -------- Allow window control --------
void allowPumpFor(unsigned long durationMs) {
  unsigned long nowMs = millis();
  allowUntil = nowMs + durationMs;
  allowActive = true;
  setInhibit(false); // allow
  mqttClient.publish("pool/sumppump/status", "allow");
}

void endAllowWindow() {
  allowActive = false;
  setInhibit(true); // inhibit
  mqttClient.publish("pool/sumppump/status", "inhibit");
}

void maybeCloseAllowWindow() {
  if (allowActive && (long)(millis() - allowUntil) >= 0) {
    endAllowWindow();
    noRearmUntil = millis() + 5UL * 60000UL; // 5 min refractory; adjust as needed
  }
}

// -------- Decision logic --------
void effectivenessCheckAlert() {
  // Check last 5 minutes drop while in allow window
  int oldLvl = 0; time_t oldTs = 0;
  if (!findReadingOlderThan(300, oldLvl, oldTs)) return;
  if (oldTs == 0) return;
  if (oldLvl - level < expectedDrop) {
    ensureWIFI(); ensureMQTT();
    mqttClient.publish("pool/sumppump/alert", "Pump not lowering water level by 2cm in 5 minutes as expected.");
  }
}

void decideFlush() {
  time_t nowSec = time(nullptr);
  struct tm *timeinfo = localtime(&nowSec);
  int hour = timeinfo ? timeinfo->tm_hour : -1;

  bool timeOK = (timeStatus() == timeSet);
  bool isNight = timeOK ? (hour >= NIGHT || hour < MORNING) : false;

  // Determine rise over window
  float rise = riseCmPerMin(RISE_WINDOW_SEC); // cm/min
  const float FAST_RISE_CMPM = 1.0f;          // tune: 1 cm/min over 5 min

  // Close existing allow window if expired
  maybeCloseAllowWindow();

  // Effectiveness alert while allowed
  if (allowActive) effectivenessCheckAlert();

  // If currently allowed, do not re-decide until window closes
  if (allowActive) return;

  // Respect refractory unless critical
  bool inRefractory = (long)(millis() - noRearmUntil) < 0;

  // Daytime policy: inhibit except critical/fast-rise override
  if (!isNight) {
    bool overrideDay = (level > criticalWaterLevel) || (rise >= FAST_RISE_CMPM);
    if (pumpOperationSafe && overrideDay) {
      if (!inRefractory) allowPumpFor(min(pumpOperationTimeout, 5UL * 60000UL)); // allow up to 5 min by default
    } else {
      // stay inhibited
      if (!isInhibited()) endAllowWindow(); // ensure inhibit state published if needed
    }
    return;
  }

  // Night policy: prefer to flush at night only if needed
  bool nightEligible = (level > waterLevelThreshold);
  if (pumpOperationSafe && nightEligible && !inRefractory) {
    allowPumpFor(pumpOperationTimeout); // full window at night
  } else {
    if (!isInhibited()) endAllowWindow();
  }
}

void loop() {
  unsigned long now = millis();
  mqttClient.loop();

  if (now - lastCheck > SLEEP) {
    ensureWIFI();
    ensureMQTT();
    lastCheck = now;
  }

  getWaterLevel();
  maybePublishLevel();
  decideFlush();

  driveLedsNonBlocking();

  events();
  ESP.wdtFeed();
}
