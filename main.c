#include <ESP8266WiFi.h>
#include <ezTime.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <WiFiUdp.h>

#define ADC_PIN A0
#define PUMP_RELAY_PIN 5
#define SLEEP 10000
#define MORNING 5
#define NIGHT 22
#define ROD_LENGTH 42
#define SOME_THRESHOLD 0.5   // TODO
#define MAX_BACKOFF 40000UL  // Maximum backoff time in milliseconds

const char* ssid = "a";
const char* password = "b";
const char* mqttServer = "c";
const int mqttPort = 1883;
const char* mqttUser = "d";
const char* mqttPassword = "e";
const char* ntpServer = "pool.ntp.org";
const long timeZoneOffset_sec = -5 * 60 * 60; // EST timezone
const int daylightOffset_sec = 3600;          // DST
const int waterLevelThreshold = 20;
const int criticalWaterLevel = 40;
const int minimumWaterLevel = 0;
const unsigned long pumpOperationTimeout = 5 * 60000;
const int expectedDrop = 2;    // We expect to drop at least 2cm in 5m
bool pumpOperationSafe = true; // Default to true, safe to operate

static unsigned long lastCheck = 0;
int raw = 0;
int level = 0;
unsigned long wifiBackoff = 3000;

WiFiClient espClient;
Timezone myTZ;
PubSubClient mqttClient(espClient);

const int lookupTable[][2] = {
  {188, 0},
  {255, 5},
  {350, 10},
  {411, 15},
  {473, 20},
  {555, 25},
  {608, 30},
  {658, 35},
  {747, 37},
  {826, 39},
  {900, 40},
  {903, 41}, 
  {980, 42}
};
const int lookupTableSize = sizeof(lookupTable) / sizeof(lookupTable[0]);

struct WaterLevelReading {
  int lvl;
  time_t timestamp;
};

const int bufferSize = 24;
WaterLevelReading readings[bufferSize];
int currentReadingIndex = 0;

void setupWIFI() {
  WiFi.begin(ssid, password);
  delay(3000);
  WiFi.reconnect();
  delay(2000);
  ensureWIFI();
}

void ensureWIFI() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.print("WIFI not connected... Retrying... ");
    
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < wifiBackoff) {
        WiFi.reconnect();
        Serial.print(".");
        delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("connected.");
      wifiBackoff = 1000;  // Reset backoff time on successful connection
    } else {
      Serial.println("attempt failed.");
      wifiBackoff = min(wifiBackoff * 2, MAX_BACKOFF);  // Exponential backoff
    }
  }
}

void setupMQTT() {
  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  ensureMQTT();
}

void ensureMQTT() {
  if (!mqttClient.connected()) {
    Serial.print("MQTT not connected... ");
    
    for (int i = 3; i > 0 && !mqttClient.connected(); i--) {
      mqttClient.connect("ESP8266Client", mqttUser, mqttPassword);
      delay(1000);
    }

    if (!mqttClient.connected()) {
     Serial.println("attempt failed.");
    } else {
      Serial.println("connected.");
      mqttClient.publish("pool/sumppump/log", "Reconnect.");
      mqttClient.subscribe("pool/sumppump/safe");
    }
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
    // Assume payload is a simple text message "yes" = safe
    String message = "";
    for (int i = 0; i < length; i++) {
      message += (char)payload[i];
    }
    if (message == "no") {
      pumpOperationSafe = false;
      Serial.println("Received safety status: unsafe to operate pump.");
    } else {
      pumpOperationSafe = true;
      Serial.println("Received safety status: safe to operate pump.");
    }
  }
}

void setup() {
  Serial.begin(9600);
  Serial.println("Booting.");

  ESP.wdtDisable();
  ESP.wdtEnable(60000);

  pinMode(LED_BUILTIN, OUTPUT);

  pinMode(PUMP_RELAY_PIN, OUTPUT);
  digitalWrite(PUMP_RELAY_PIN, LOW);

  setupWIFI();

  myTZ.setLocation(F("America/New_York"));
  setServer(ntpServer); 
  setInterval(3600);

  setupMQTT();
}

float calculateImmediateSlope() {
  if (bufferSize < 2) return 0;
  
  int latestIndex = (currentReadingIndex - 1 + bufferSize) % bufferSize;
  int previousIndex = (latestIndex - 1 + bufferSize) % bufferSize;
  
  float levelChange = readings[latestIndex].lvl - readings[previousIndex].lvl;
  long timeChange = readings[latestIndex].timestamp - readings[previousIndex].timestamp;
  
  if (timeChange == 0) return 0; // Avoid division by zero
  
  return levelChange / timeChange;
}

int interpolateWaterLevel(int raw) {
  if (raw <= lookupTable[0][0]) return lookupTable[0][1];
  if (raw >= lookupTable[lookupTableSize - 1][0]) return lookupTable[lookupTableSize - 1][1];

  for (int i = 0; i < lookupTableSize - 1; i++) {
    if (raw == lookupTable[i][0]) {
      return ROD_LENGTH - lookupTable[i][1];
    }
    if (raw > lookupTable[i][0] && raw < lookupTable[i + 1][0]) {
      float slope = (float)(lookupTable[i + 1][1] - lookupTable[i][1]) / (lookupTable[i + 1][0] - lookupTable[i][0]);
      float intercept = lookupTable[i][1] - slope * lookupTable[i][0];
      return ROD_LENGTH - (slope * raw + intercept);
    }
  }
  return -1;
}

void updateBuffer(float level) {
  Serial.print("Water Level: ");
  Serial.print(level);
  Serial.println(" cm");

  time_t now = time(nullptr);
  readings[currentReadingIndex].lvl = level;
  readings[currentReadingIndex].timestamp = now;
  currentReadingIndex = (currentReadingIndex + 1) % bufferSize; // Move to the next index, wrap around if at the end
}

void getWaterLevel(bool update = true) {
  time_t now = time(nullptr);
  level = interpolateWaterLevel(analogRead(ADC_PIN));
  delay(500);
  if (update) { updateBuffer(level); } // Update the circular buffer with the new reading
}

void activatePump() {
  unsigned long startMillis = millis();
  getWaterLevel();
  int initialLevel = level;
  int lastCheckedLevel = level;

  digitalWrite(PUMP_RELAY_PIN, HIGH);
  Serial.println("Pump activated, flushing started...");

  while (true) {
    ESP.wdtFeed();
    unsigned long currentMillis = millis();
    getWaterLevel(false);

    if (level <= minimumWaterLevel) {
      Serial.println("Minimum water level reached. Pump deactivated.");
      break;
    }
    // Check every 5 minutes if the level has decreased
    if (currentMillis - startMillis > 300000) {
      if (level >= lastCheckedLevel - expectedDrop) {
        Serial.println("Insufficient water level drop observed. Pump deactivated.");

        mqttClient.publish("pool/sumppump/alert", "Pump not lowering water level in 2m as expected.");
        break;
      }
      lastCheckedLevel = level;
      startMillis = currentMillis;
    }
  }
  digitalWrite(PUMP_RELAY_PIN, LOW);
  delay(500);
}

void decideFlush() {
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);
  int hour = timeinfo->tm_hour;
  bool pump = false;
  bool publishSuccessfully;

  float immediateSlope = calculateImmediateSlope();

  if (pumpOperationSafe && ((hour >= NIGHT || hour < MORNING) || level > criticalWaterLevel || immediateSlope > SOME_THRESHOLD)) {
    activatePump();
    pump = true;
  }

  String pumpStatus = pump ? "on" : "off";
  publishSuccessfully = mqttClient.publish("pool/sumppump/status", pumpStatus.c_str());
  if (!publishSuccessfully) {
    Serial.println("Failed to publish the pump status.");
  }

  publishSuccessfully = mqttClient.publish("pool/sumppump/level", String(level).c_str());
  if (!publishSuccessfully) {
    Serial.println("Failed to publish the water level.");
  }
}

void loop() {
  unsigned long now = millis();

  if (now - lastCheck > SLEEP) {
    ensureWIFI();
    ensureMQTT();
    mqttClient.loop();
    lastCheck = now;
  }

  getWaterLevel();
  decideFlush();
  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN)); /////////////////////////////////////////////////////

  events();
  ESP.wdtFeed();
  delay(1000);
}
