/* ============================================================================
 * FlushWaterNG — sump pump controller, Seeed XIAO ESP32-C3
 *
 * BUILD
 *   Board: XIAO_ESP32C3.  USB CDC On Boot: Enabled (else Serial goes to the
 *   GPIO20/21 UART and the monitor stays empty).  Flash Mode: DIO.
 *   Libraries: PubSubClient, ezTime.
 *   Copy config.example.h to config.h and fill in credentials.
 *
 * WIRING (XIAO silkscreen labels)
 *   D1  <- divider node, via 10k series + 100nF to GND
 *   D10 -> relay inhibit/allow input
 *   D5  -> LED anode -> 220R -> GND
 *
 * HARDWARE CONSTRAINTS — do not skip these
 *   ADC1 only: analogRead works on D0/D1/D2. D3 is ADC2 and returns garbage
 *   while WiFi is running.
 *
 *   The ADC pin must never see more than 3.3V. An open sender (broken wire,
 *   corroded connector) pulls the divider node to the full supply. The 10k
 *   series resistor plus a BAT54 Schottky from the pin to 3V3 clamps it.
 *
 *   10k pull-up from D10 to 3V3. INHIBIT_ACTIVE_LEVEL is HIGH, so the pin
 *   floating low at boot means pump ALLOWED — the wrong failure direction.
 *   Internal pull-ups are inactive during reset, so it must be external.
 *
 *   External supply to the 5V pin OR USB, never both: that pin is USB VBUS.
 *   Power the relay module separately; the coil draws 70-90 mA and switching
 *   it injects noise into the ADC rail.
 * ========================================================================= */

#include <WiFi.h>
#include <WiFiUdp.h>
#include <WiFiClient.h>
#include <PubSubClient.h>
#include <ezTime.h>
#include <limits.h>
#include <esp_task_wdt.h>
#include <esp_system.h>

// Wi-Fi + MQTT credentials live in config.h, which is git-ignored.
// Copy config.example.h to config.h and fill in your own values.
#include "config.h"

// ------------------------------------------------- pins (XIAO ESP32C3) ----
// XIAO ESP32C3 D-number to GPIO mapping (NOT the same as NodeMCU's):
//   D0 = GPIO2  (ADC1_CH2, strapping)   D6  = GPIO21 (UART TX)
//   D1 = GPIO3  (ADC1_CH3)              D7  = GPIO20 (UART RX)
//   D2 = GPIO4  (ADC1_CH4, MTMS)        D8  = GPIO8  (SCK, strapping)
//   D3 = GPIO5  (ADC2_CH0, MTDI)        D9  = GPIO9  (MISO, strapping/BOOT btn)
//   D4 = GPIO6  (SDA, MTCK)             D10 = GPIO10 (MOSI)
//   D5 = GPIO7  (SCL, MTDO)
// Avoid D0, D8, D9 (strapping) and D6/D7 (UART0). The D* constants come from
// the XIAO_ESP32C3 board variant, so select that board in the IDE.
#define ADC_PIN         D1    // GPIO3, ADC1. Sensor input. Use only D0/D1/D2.
#define PUMP_RELAY_PIN  D10   // GPIO10, no alternate function — cleanest output
#define STATUS_LED_PIN  D5    // GPIO7. THE status LED. Anode -> 220R -> D5.

// The XIAO ESP32C3 has NO user-controllable onboard LED — the only LED on the
// board is the battery charge indicator, hard-wired to VCC_3V3. LED_BUILTIN is
// not defined for this variant, so the status LED must be external.

#define INHIBIT_ACTIVE_LEVEL HIGH   // set to LOW if your relay logic is inverted
#define SLEEP        10000
#define MORNING      5
#define NIGHT        22
#define ROD_LENGTH   42             // informational; level now comes from ohms
#define MAX_BACKOFF  40000UL

// ------------------------------------------------------------- config ----
const char* ssid         = WIFI_SSID;
const char* password     = WIFI_PASSWORD;
const char* mqttServer   = MQTT_SERVER;
const int   mqttPort     = MQTT_PORT;
const char* mqttUser     = MQTT_USER;
const char* mqttPassword = MQTT_PASSWORD;
const char* ntpServer    = NTP_SERVER;

const int waterLevelThreshold = 5;    // night eligibility
const int criticalWaterLevel  = 32;   // day threshold
const int minimumWaterLevel   = 0;

const unsigned long pumpOperationTimeout   = 5UL * 60000UL;

// Pump effectiveness, as a rate so it stays meaningful whichever length the
// allow window actually ran.
const float         MIN_DROP_CM_PER_MIN    = 0.4f;
const unsigned long EFFECTIVENESS_GRACE_MS = 120000UL;  // must be < pumpOperationTimeout
const unsigned long LEVEL_PUB_PERIOD_MS    = 1200000UL;
const int           LEVEL_PUB_DELTA_CM     = 2;
const unsigned long RISE_WINDOW_SEC        = 300;

/* Resilience timers.
 *   SAFE_FLAG_TTL_MS  an MQTT "no" latches pumpOperationSafe false. If the
 *                     broker then dies the pump would stay inhibited forever
 *                     and the sump floods, so the latch expires and fails open.
 *   MAX_OFFLINE_MS    the task WDT cannot catch "offline forever" — loop()
 *                     keeps running and feeding it. Only a wall-clock timer
 *                     catches a wedged WiFi stack.                           */
bool          pumpOperationSafe = true;
unsigned long lastSafeMsgMs     = 0;
const unsigned long SAFE_FLAG_TTL_MS = 30UL * 60000UL;   // expire an unsafe latch
unsigned long lastOnlineMs      = 0;
const unsigned long MAX_OFFLINE_MS   = 15UL * 60000UL;   // reboot after this
unsigned long lastHeartbeatMs   = 0;
const unsigned long HEARTBEAT_MS     = 5UL * 60000UL;
unsigned long lastLevelPubMs    = 0;
int           lastLevelPubCm    = INT_MIN;
static unsigned long lastCheck  = 0;
static unsigned long lastSample = 0;
const unsigned long SAMPLE_PERIOD_MS = 1000;   // how often to read the sender
int           level             = 0;
unsigned long wifiBackoff       = 3000;

WiFiClient   espClient;
Timezone     myTZ;
PubSubClient mqttClient(espClient);

bool          allowActive     = false;
unsigned long ledPatternStart = 0;

unsigned long allowUntil    = 0;
unsigned long allowMinUntil = 0;   // earliest the window may close on drain
unsigned long noRearmUntil  = 0;
unsigned long allowStartMs       = 0;      // when the current window opened
int           levelAtAllowStart  = 0;      // level when it opened
bool          effectivenessAlerted = false; // one alert per window, not per second
const unsigned long MIN_ALLOW_MS = 30000;   // anti-chatter floor on the relay

/* ===================== SENSOR FRONT END =====================================
 * The sender is a resistive level sender (240 ohm empty -> 33 ohm full, the
 * standard US automotive range), wired as the BOTTOM leg of a divider:
 *
 *     SUPPLY --[ R_TOP ]--+-- node --[10k]--+-- ADC_PIN (D1)
 *                         |                 |
 *                     [ sender ]         [100nF]
 *                         |                 |
 *                        GND               GND
 *
 * Level is looked up by SENDER RESISTANCE, not by ADC counts or millivolts.
 * Resistance is what the float actually varies, so the table survives a change
 * of supply voltage, top resistor, ADC reference or chip.
 *
 * Measure both of these with a multimeter rather than trusting the markings,
 * and measure SUPPLY_MV on the source you will actually run on — USB VBUS and
 * an external brick do not read the same.                                    */
const float SUPPLY_MV = 5000.0f;    // actual supply at the top of the divider
const float R_TOP_OHM = 1200.0f;    // actual top resistor
/* ---------------------------------------------------------------------------
 * Sender resistance -> water level in cm, from a measured sweep.
 * MUST be strictly ASCENDING in resistance and DESCENDING in level;
 * validateSenderTable() enforces that at boot.
 *
 * The sender is not linear: ~3.5 ohm/cm through the main body, but ~12 ohm/cm
 * below 7 cm and steeper still in the last centimetre. Keep the dense rows at
 * the bottom — that is where the pump decisions happen.                      */
const float senderTable[][2] = {
  { 36.0f, 42.0f},   // full   — 42 cm water
  { 43.2f, 39.0f},
  { 50.3f, 37.0f},
  { 58.6f, 35.0f},
  { 65.3f, 33.0f},
  { 73.0f, 30.0f},
  { 80.8f, 28.0f},
  { 87.5f, 26.0f},
  { 94.7f, 24.0f},
  {103.3f, 22.0f},
  {110.3f, 20.0f},
  {117.8f, 18.0f},
  {124.4f, 16.0f},
  {132.1f, 14.0f},
  {139.7f, 12.0f},
  {147.7f,  9.0f},
  {154.7f,  7.0f},
  {180.3f,  5.0f},
  {206.4f,  3.0f},
  {231.4f,  1.0f},
  {262.3f,  0.0f},   // empty  —  0 cm water
};
const int senderTableSize = sizeof(senderTable) / sizeof(senderTable[0]);
bool senderTableValid = true;

/* A malformed table does not crash: levelFromOhms() silently returns whatever
 * the first bracketing pair gives. Catch it loudly at boot instead. */
bool validateSenderTable() {
  bool ok = true;
  for (int i = 0; i < senderTableSize - 1; i++) {
    if (senderTable[i+1][0] <= senderTable[i][0]) {
      Serial.printf("TABLE ERROR: row %d (%.1f ohm) does not exceed row %d (%.1f ohm)\n",
                    i+1, senderTable[i+1][0], i, senderTable[i][0]);
      ok = false;
    }
    if (senderTable[i+1][1] >= senderTable[i][1]) {
      Serial.printf("TABLE ERROR: row %d (%.1f cm) is not below row %d (%.1f cm)\n",
                    i+1, senderTable[i+1][1], i, senderTable[i][1]);
      ok = false;
    }
  }
  return ok;
}

// Fault thresholds on the COMPUTED resistance, not on raw ADC.
const float R_SHORT_OHM = 15.0f;    // below this: shorted sender / wiring
const float R_OPEN_OHM  = 400.0f;   // above this: open sender / broken wire

// Set to 1 to print raw mV and computed ohms every cycle while calibrating.
#define CALIBRATION_VERBOSE 0

struct WaterLevelReading {
  int    lvl;
  time_t timestamp;
};
const int bufferSize = 24;
WaterLevelReading readings[bufferSize];
int currentReadingIndex = 0;

// Forward declarations — required if you ever move this into a .cpp file,
// where the IDE's automatic prototype generation does not apply.
void mqttCallback(char* topic, byte* payload, unsigned int length);
void ensureWIFI();
void ensureMQTT();
void setInhibit(bool inhibit);
bool isInhibited();
void maybeCloseAllowWindow();   // called from ensureWIFI's blocking wait
void publishDiagnostics(const char* why);

// ----------------------------------------------------------- watchdog ----
static void wdtSetup(uint32_t timeoutMs) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {
    .timeout_ms     = timeoutMs,
    .idle_core_mask = 0,
    .trigger_panic  = true,
  };
  // The Arduino core already inits the TWDT, so reconfigure rather than init.
  if (esp_task_wdt_reconfigure(&cfg) != ESP_OK) esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(timeoutMs / 1000, true);
#endif
  esp_task_wdt_add(NULL);   // watch the Arduino loop task
}

static inline void wdtFeed() { esp_task_wdt_reset(); }

/* BROWNOUT is the one to watch: a pump motor starting sags a shared supply,
 * and without this it just looks like a random reboot. */
const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "power-on";
    case ESP_RST_EXT:      return "external-pin";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "PANIC/exception";
    case ESP_RST_INT_WDT:  return "interrupt-WDT";
    case ESP_RST_TASK_WDT: return "task-WDT";
    case ESP_RST_WDT:      return "other-WDT";
    case ESP_RST_DEEPSLEEP:return "deep-sleep";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
  }
}

void checkConnectivityWatchdog(unsigned long now) {
  if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
    lastOnlineMs = now;
    return;
  }
  if (now - lastOnlineMs < MAX_OFFLINE_MS) return;

  // Never reboot mid-flush: that stops the pump while water is still rising.
  if (allowActive) return;

  Serial.println("Offline too long — rebooting to clear the network stack.");
  setInhibit(true);          // defined state before we go down
  Serial.flush();
  delay(200);
  esp_restart();
}

void expireStaleSafetyFlag(unsigned long now) {
  if (pumpOperationSafe) return;
  if (now - lastSafeMsgMs < SAFE_FLAG_TTL_MS) return;

  pumpOperationSafe = true;
  lastSafeMsgMs = now;
  Serial.println("Safety flag STALE — no MQTT update in 30 min, failing open.");
  if (mqttClient.connected())
    mqttClient.publish("pool/sumppump/alert",
      "Safety hold expired after 30 min with no broker update; pump re-enabled.");
}

// --------------------------------------------------------------- wifi ----
void setupWIFI() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);          // avoids multi-second MQTT stalls on the C3
  WiFi.begin(ssid, password);

  const unsigned long deadline = millis() + 20000UL;
  while (WiFi.status() != WL_CONNECTED && (long)(millis() - deadline) < 0) {
    delay(50);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi up: "); Serial.println(WiFi.localIP());
    wifiBackoff = 2000;
  } else {
    Serial.println("WiFi boot connect timeout; will retry in loop.");
  }
}

void ensureWIFI() {
  if (WiFi.status() == WL_CONNECTED) return;

  Serial.print("WIFI not connected... Retrying... ");
  // On ESP32, disconnect()+begin() is far more reliable than reconnect()
  // when the AP has dropped us. Do it once, then wait — do not spam it.
  WiFi.disconnect();
  WiFi.begin(ssid, password);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < wifiBackoff) {
    delay(250);
    Serial.print(".");
    wdtFeed();
    // This can block for up to MAX_BACKOFF (40 s). Keep the safety timer
    // honest across it, or an allow window overruns its deadline by most of
    // a minute purely because the AP went away.
    maybeCloseAllowWindow();
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("connected.");
    wifiBackoff = 2000;
  } else {
    Serial.println("attempt failed.");
    wifiBackoff = min(wifiBackoff * 2, MAX_BACKOFF);
  }
}

// --------------------------------------------------------------- mqtt ----
void setupMQTT() {
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);      // default 15 s is twitchy over flaky WiFi
  mqttClient.setBufferSize(512);    // default 256 truncates the longer alerts
  ensureMQTT();
}

void publishDiagnostics(const char* why) {
  if (!mqttClient.connected()) return;
  char buf[192];
  snprintf(buf, sizeof(buf),
           "%s reset=%s ip=%s rssi=%d heap=%u uptime=%lus level=%dcm %s",
           why, resetReasonStr(),
           WiFi.localIP().toString().c_str(), WiFi.RSSI(),
           (unsigned)ESP.getFreeHeap(), millis() / 1000UL, level,
           allowActive ? "ALLOW" : "inhibit");
  mqttClient.publish("pool/sumppump/log", buf);
  Serial.println(buf);
}

void ensureMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (mqttClient.connected()) return;

  Serial.print("MQTT not connected... ");
  for (int i = 3; i > 0 && !mqttClient.connected(); i--) {
    // ESP.getChipId() does not exist on ESP32. Low 24 bits of the eFuse MAC
    // is the closest equivalent and is unique per device.
    String cid = String("ESP32C3-") +
                 String((uint32_t)(ESP.getEfuseMac() & 0xFFFFFFUL), HEX);
    mqttClient.connect(cid.c_str(), mqttUser, mqttPassword);
    for (int k = 0; k < 10; k++) { mqttClient.loop(); delay(50); }
    wdtFeed();
  }
  if (!mqttClient.connected()) {
    Serial.println("attempt failed.");
  } else {
    Serial.println("connected.");
    mqttClient.subscribe("pool/sumppump/safe");
    // Retained, so Home Assistant resolves our state after a broker or
    // controller restart instead of sitting at "unknown".
    mqttClient.publish("pool/sumppump/status",
                       allowActive ? "allow" : "inhibit", true);
    publishDiagnostics("connected");
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
      Serial.println("attempt failed. Default: noon, January 1, 2020.");
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
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
    pumpOperationSafe = (message != "no");
    lastSafeMsgMs = millis();      // resets the staleness timer
    Serial.println(pumpOperationSafe ? "Safety status: safe to operate pump."
                                     : "Safety status: unsafe to operate pump.");
  }
}

void maybePublishLevel() {
  if (!mqttClient.connected()) return;
  unsigned long nowMs = millis();
  bool timeElapsed = (nowMs - lastLevelPubMs) >= LEVEL_PUB_PERIOD_MS;
  bool bigDelta    = (lastLevelPubCm == INT_MIN) ||
                     (abs(level - lastLevelPubCm) >= LEVEL_PUB_DELTA_CM);
  if (timeElapsed || bigDelta) {
    if (mqttClient.publish("pool/sumppump/level", String(level).c_str())) {
      lastLevelPubMs = nowMs;
      lastLevelPubCm = level;
    }
  }
}

void setInhibit(bool inhibit) {
  digitalWrite(PUMP_RELAY_PIN, inhibit ? INHIBIT_ACTIVE_LEVEL
                                       : !INHIBIT_ACTIVE_LEVEL);
}

bool isInhibited() {
  return digitalRead(PUMP_RELAY_PIN) == INHIBIT_ACTIVE_LEVEL;
}

// -------------------------------------------------------------- setup ----
void setup() {
  Serial.begin(115200);
  delay(500);              // let USB CDC enumerate before the first print
  Serial.print("Booting. Last reset: ");
  Serial.println(resetReasonStr());

  unsigned long bootMs = millis();
  lastOnlineMs  = bootMs;   // do not reboot instantly on a slow first connect
  lastSafeMsgMs = bootMs;

  // Relay first, and safe, before anything that can block.
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  setInhibit(true);

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);

  /* ESP32-C3 attenuation ranges (C3 values; ESP32-classic differs):
   *   0 dB 0-750 mV | 2.5 dB 0-1050 | 6 dB 0-1300 | 11 dB 0-2500
   * Signal is 134-833 mV. 6 dB rather than 2.5 dB because the C3's internal
   * Vref varies 1000-1200 mV chip to chip, which would leave a worst-case
   * part only ~14% headroom at 2.5 dB. Resolution is not the constraint:
   * 0.32 mV/LSB is ~52 counts per cm, far finer than the sender resolves. */
  analogReadResolution(12);
  analogSetPinAttenuation(ADC_PIN, ADC_6db);

  senderTableValid = validateSenderTable();
  if (!senderTableValid) {
    Serial.println("!! senderTable is malformed. Level readings CANNOT be trusted.");
    Serial.println("!! Failing safe: pump will be permanently ALLOWED.");
  }

  wdtSetup(60000);

  setupWIFI();

  myTZ.setLocation(F(TZ_LOCATION));
  setServer(ntpServer);
  setInterval(3600);
  setDebug(INFO);
  setupNTP();

  setupMQTT();
  ledPatternStart = millis();

  Serial.print("Free heap: ");
  Serial.println(ESP.getFreeHeap());
}

// ------------------------------------------- level / interpolation ----
/* Convert the divider node voltage back to sender resistance.
 *   V = SUPPLY * R / (R_TOP + R)   =>   R = R_TOP * V / (SUPPLY - V)
 * Returns -1 on a nonsensical reading (V at or above the supply). */
float ohmsFromMillivolts(float mv) {
  if (mv >= SUPPLY_MV - 1.0f) return -1.0f;   // open sender, or bad SUPPLY_MV
  if (mv <= 0.0f) return 0.0f;
  return R_TOP_OHM * mv / (SUPPLY_MV - mv);
}

int levelFromOhms(float r) {
  if (r <= senderTable[0][0])                  return (int)senderTable[0][1];
  if (r >= senderTable[senderTableSize-1][0])  return (int)senderTable[senderTableSize-1][1];

  for (int i = 0; i < senderTableSize - 1; i++) {
    float r0 = senderTable[i][0],     r1 = senderTable[i+1][0];
    float l0 = senderTable[i][1],     l1 = senderTable[i+1][1];
    if (r >= r0 && r <= r1) {
      float t = (r - r0) / (r1 - r0);
      return (int)lroundf(l0 + t * (l1 - l0));
    }
  }
  return -1;
}

void updateBuffer(int lvl) {
  time_t now = time(nullptr);
  readings[currentReadingIndex].lvl       = lvl;
  readings[currentReadingIndex].timestamp = now;
  currentReadingIndex = (currentReadingIndex + 1) % bufferSize;
}

void getWaterLevel(bool update = true) {
  // 16 samples: the C3 ADC shows spike-like errors during WiFi TX.
  long acc = 0;
  for (int i = 0; i < 16; i++) { acc += analogReadMilliVolts(ADC_PIN); delay(3); }
  float mv = acc / 16.0f;

  float ohms = ohmsFromMillivolts(mv);

#if CALIBRATION_VERBOSE
  Serial.print("  [cal] "); Serial.print(mv, 1); Serial.print(" mV -> ");
  if (ohms < 0) Serial.println("OPEN");
  else { Serial.print(ohms, 1); Serial.println(" ohm"); }
#endif

  if (!senderTableValid || ohms < 0 || ohms < R_SHORT_OHM || ohms > R_OPEN_OHM) {
    Serial.print("WARNING: sender out of range ("); Serial.print(mv, 1);
    Serial.println(" mV) — allowing pump");
    if (isInhibited()) setInhibit(false);
    ensureWIFI();
    ensureMQTT();
    mqttClient.publish("pool/sumppump/alert", "Sensor out of range, pump allowed.");
    return;   // do not update level or buffer on a bad reading
  }

  level = levelFromOhms(ohms);
  Serial.print("Water Level: "); Serial.print(level); Serial.println(" cm");
  if (update) updateBuffer(level);
}

bool findReadingOlderThan(unsigned long windowSec, int& outLvl, time_t& outTs) {
  time_t nowSec = time(nullptr);
  time_t target = nowSec - (time_t)windowSec;

  for (int i = 0; i < bufferSize; i++) {
    int idx = (currentReadingIndex - 1 - i + bufferSize) % bufferSize;
    time_t ts = readings[idx].timestamp;
    if (ts == 0) continue;
    if (ts <= target) { outLvl = readings[idx].lvl; outTs = ts; return true; }
  }
  for (int i = 0; i < bufferSize; i++) {
    int idx = (currentReadingIndex - 1 - i + bufferSize) % bufferSize;
    time_t ts = readings[idx].timestamp;
    if (ts != 0) { outLvl = readings[idx].lvl; outTs = ts; return true; }
  }
  return false;
}

float riseCmPerMin(unsigned long windowSec) {
  int oldLvl = 0; time_t oldTs = 0;
  if (!findReadingOlderThan(windowSec, oldLvl, oldTs)) return 0.0f;
  time_t nowSec = time(nullptr);
  if (nowSec <= oldTs) return 0.0f;
  float delta   = (float)level - (float)oldLvl;
  float minutes = (nowSec - oldTs) / 60.0f;
  if (minutes <= 0.0f) return 0.0f;      // guard against divide-by-zero
  return delta / minutes;
}

// ---------------------------------------------------------- LED pattern ----
/* One LED, as a COUNTED BLINK CODE: N short pulses then a long dark gap.
 * Counting pulses is far easier than judging blink rates by eye.
 *
 *   SOLID ON   pump ALLOWED right now (allow window open)
 *   1 blip     all good, idle — normal heartbeat
 *   2 blips    no WiFi
 *   3 blips    WiFi up, but no MQTT broker
 *   4 blips    connected, but clock not synced to NTP
 *   5 blips    MQTT reported unsafe to operate
 *   6 blips    senderTable malformed — readings invalid, pump forced ALLOW
 *   DARK       firmware not running (crashed, or held in reset)
 *
 * Only the highest-priority active condition is shown, and codes ascend with
 * severity — more blinking is worse.                                        */
#define LED_CODE_OK       1
#define LED_CODE_NO_WIFI  2
#define LED_CODE_NO_MQTT  3
#define LED_CODE_NO_TIME  4
#define LED_CODE_UNSAFE   5
#define LED_CODE_BAD_TABLE 6

const unsigned long LED_PULSE_ON_MS  = 150;   // length of one blip
const unsigned long LED_PULSE_OFF_MS = 200;   // dark time between blips
const unsigned long LED_GAP_MS       = 1200;  // dark gap that ends the group

int getLedCode(bool& solidOn) {
  bool wifiOK = (WiFi.status() == WL_CONNECTED);
  bool mqttOK = mqttClient.connected();
  bool timeOK = (timeStatus() == timeSet);

  solidOn = false;
  if (!senderTableValid)  return LED_CODE_BAD_TABLE;   // outranks everything
  if (allowActive)        { solidOn = true; return 0; }
  if (!pumpOperationSafe) return LED_CODE_UNSAFE;
  if (!wifiOK)            return LED_CODE_NO_WIFI;
  if (!mqttOK)            return LED_CODE_NO_MQTT;
  if (!timeOK)            return LED_CODE_NO_TIME;
  return LED_CODE_OK;
}

void driveLedNonBlocking() {
  static int lastCode = -1;
  unsigned long nowMs = millis();

  bool solidOn;
  int code = getLedCode(solidOn);

  if (solidOn) {
    digitalWrite(STATUS_LED_PIN, HIGH);
    ledPatternStart = nowMs;
    lastCode = -1;              // force a clean restart when solid ends
    return;
  }

  // Restart the group from its first pulse whenever the state changes,
  // so you never catch a half-finished count and miscount it.
  if (code != lastCode) {
    lastCode = code;
    ledPatternStart = nowMs;
  }

  const unsigned long slotMs  = LED_PULSE_ON_MS + LED_PULSE_OFF_MS;
  const unsigned long cycleMs = (unsigned long)code * slotMs + LED_GAP_MS;
  unsigned long phase = (nowMs - ledPatternStart) % cycleMs;

  bool on = (phase < (unsigned long)code * slotMs) &&
            ((phase % slotMs) < LED_PULSE_ON_MS);
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
}

// ------------------------------------------------- allow window control ----
void allowPumpFor(unsigned long durationMs) {
  unsigned long nowMs = millis();
  allowUntil          = nowMs + durationMs;
  allowMinUntil       = nowMs + MIN_ALLOW_MS;
  allowStartMs        = nowMs;
  levelAtAllowStart   = level;      // the baseline the drop is measured from
  effectivenessAlerted = false;
  allowActive         = true;
  setInhibit(false);
  mqttClient.publish("pool/sumppump/status", "allow", true);     // retained
}

void endAllowWindow() {
  allowActive = false;
  setInhibit(true);
  mqttClient.publish("pool/sumppump/status", "inhibit", true);   // retained
}

/* Closes on timeout OR once drained, whichever comes first. The 30 s floor
 * stops the relay chattering if the reading hovers at the threshold. */
void maybeCloseAllowWindow() {
  if (!allowActive) return;
  unsigned long nowMs = millis();

  bool timedOut = (long)(nowMs - allowUntil) >= 0;
  bool drained  = (level <= minimumWaterLevel) &&
                  ((long)(nowMs - allowMinUntil) >= 0);

  if (timedOut || drained) {
    Serial.println(drained ? "Allow window closed: sump drained."
                           : "Allow window closed: timeout.");
    endAllowWindow();
    noRearmUntil = nowMs + 5UL * 60000UL;
  }
}

// ------------------------------------------------------ decision logic ----
/* At most once per allow window, after the pump has had the grace period to
 * move water. Reaching minimumWaterLevel counts as success however slow. */
void effectivenessCheckAlert() {
  if (effectivenessAlerted) return;
  if (level <= minimumWaterLevel) return;          // drained = success

  unsigned long elapsed = millis() - allowStartMs;
  if (elapsed < EFFECTIVENESS_GRACE_MS) return;    // too early to judge

  float minutes = elapsed / 60000.0f;
  float dropped = (float)(levelAtAllowStart - level);
  float rate    = dropped / minutes;

  if (rate < MIN_DROP_CM_PER_MIN) {
    effectivenessAlerted = true;                   // do not repeat this window
    ensureWIFI(); ensureMQTT();
    char msg[160];
    snprintf(msg, sizeof(msg),
             "Pump ineffective: %.0f cm in %.1f min (%.2f cm/min, expected %.2f). "
             "Level %d cm.",
             dropped, minutes, rate, MIN_DROP_CM_PER_MIN, level);
    Serial.println(msg);
    mqttClient.publish("pool/sumppump/alert", msg);
  }
}

/* THE RULES, in one place:
 *
 *   NIGHT (22:00-04:59)  flush when level > waterLevelThreshold (5 cm)
 *   DAY   (05:00-21:59)  flush only when CRITICAL:
 *                          level > criticalWaterLevel (32 cm)
 *                          OR rising >= FAST_RISE_CMPM (1.0 cm/min)
 *   ALWAYS REQUIRED      pumpOperationSafe (MQTT has not said "no")
 *   REFRACTORY           5 min lockout after a window closes,
 *                        BYPASSED when critical
 *   WINDOW               5 min max, closes early once level <= 0 cm
 *                        (never before MIN_ALLOW_MS, to stop relay chatter)
 *
 * Both failure directions are deliberate:
 *   - UNKNOWN TIME FALLS BACK TO NIGHT, so a WiFi or NTP outage cannot quietly
 *     disarm flood protection by leaving us in the restrictive daytime mode.
 *     Pumping at an inconvenient hour is far cheaper than flooding.
 *   - CRITICAL BYPASSES THE REFRACTORY, so a real flood is not locked out for
 *     5 of every 10 minutes.                                                 */
const float FAST_RISE_CMPM = 1.0f;

void decideFlush() {
  time_t nowSec = time(nullptr);
  struct tm *timeinfo = localtime(&nowSec);
  int hour = timeinfo ? timeinfo->tm_hour : -1;

  bool timeOK  = (timeStatus() == timeSet);
  bool isNight = timeOK ? (hour >= NIGHT || hour < MORNING) : true;

  float rise = riseCmPerMin(RISE_WINDOW_SEC);

  maybeCloseAllowWindow();
  if (allowActive) { effectivenessCheckAlert(); return; }

  bool inRefractory = (long)(millis() - noRearmUntil) < 0;
  bool critical     = (level > criticalWaterLevel) || (rise >= FAST_RISE_CMPM);
  bool blocked      = inRefractory && !critical;
  bool eligible     = isNight ? (level > waterLevelThreshold) : critical;

  if (pumpOperationSafe && eligible && !blocked) {
    allowPumpFor(pumpOperationTimeout);
    return;
  }

  // Say WHY we are not flushing, but only when the reason changes.
  static const char* lastReason = nullptr;
  const char* reason;
  if (!pumpOperationSafe)   reason = "MQTT says unsafe";
  else if (blocked)         reason = "in 5 min refractory (not critical)";
  else if (!eligible && isNight) reason = "night, but level <= threshold";
  else if (!eligible)       reason = "day, and not critical";
  else                      reason = "unknown";

  if (reason != lastReason) {
    lastReason = reason;
    Serial.printf("No flush: %s  [level %d cm, rise %.2f cm/min, %s, %s]\n",
                  reason, level, rise,
                  isNight ? "night" : "day",
                  timeOK ? "clock ok" : "NO CLOCK -> using night rules");
  }

  if (!isInhibited()) endAllowWindow();
}

// --------------------------------------------------------------- loop ----
/* The loop spins every 10 ms so the LED stays smooth, while the sender is
 * sampled only once a second. A slower loop aliases the blink pulses and
 * makes the counted code unreadable. Blink timing comes from absolute
 * millis(), so an occasional slow pass does not accumulate drift. */
void loop() {
  unsigned long now = millis();
  mqttClient.loop();

  if (now - lastCheck > SLEEP) {
    ensureWIFI();
    ensureMQTT();
    expireStaleSafetyFlag(now);      // a stuck "unsafe" latch must not persist
    checkConnectivityWatchdog(now);  // reboot a wedged network stack
    lastCheck = now;
  }

  if (now - lastHeartbeatMs >= HEARTBEAT_MS) {
    publishDiagnostics("heartbeat");
    lastHeartbeatMs = now;
  }

  if (now - lastSample >= SAMPLE_PERIOD_MS) {
    lastSample = now;
    getWaterLevel();
    maybePublishLevel();
    decideFlush();
  }

  driveLedNonBlocking();   // every pass — this is what needs the fast loop
  events();                // ezTime housekeeping
  wdtFeed();
  delay(10);
}