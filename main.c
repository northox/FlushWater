#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <WiFiClient.h>
#include <time.h>

#define ADC_PIN A0
#define PUMP_RELAY_PIN 12
#define SLEEP 500
#define MORNING 5
#define NIGHT 22
#define ROD_LENGTH 42

#define SOME_THRESHOLD 0.5 // TODO

const char* ssid = "x";
const char* password = "y";
const char* mqttServer = "z";
const int mqttPort = 1883;
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -5 * 60 * 60; // EST timezone
const int daylightOffset_sec = 3600;     // DST if applicable
const int waterLevelThreshold = 30;
const int criticalWaterLevel = 40;
const int minimumWaterLevel = 10;

int raw = 0;
int buf = 0;

WiFiClient espClient;
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
  float level;
  time_t timestamp;
};

const int bufferSize = 24; // Size of the circular buffer
WaterLevelReading readings[bufferSize]; // Circular buffer
int currentReadingIndex = 0; // Current index for inserting the next reading

void setup_wifi() {
  Serial.print("Connecting to WiFi...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nConnected to WiFi");
}

void setup_time() {
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  int maxAttempts = 5;
  time_t now = time(nullptr);
  while (now < 24 * 3600 && maxAttempts > 0) {
    Serial.println("Waiting for NTP time sync...");
    delay(1000);
    now = time(nullptr);
    maxAttempts--;
  }

  if (now < 24 * 3600) {
    Serial.println("Failed to synchronize time. Check your NTP server settings.");
    // Handle the failure appropriately @TODO
  } else {
    struct tm* timeinfo = localtime(&now);
    char buffer[26];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    Serial.print("Time acquired: ");
    Serial.println(buffer);
  }
}

void setup_mqtt() {
  mqttClient.setServer(mqttServer, mqttPort);
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  digitalWrite(PUMP_RELAY_PIN, LOW);

  Serial.begin(9600);
  setup_wifi();
  setup_time();
  setup_mqtt();
}

void reconnect() {
  int maxAttempts = 5;
  while (!mqttClient.connected() && maxAttempts > 0) {
    Serial.print("Attempting MQTT connection...");
    if (mqttClient.connect("ESP8266Client")) {
      Serial.println("connected");
    } else {
      Serial.print("failed, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
    maxAttempts--;
  }
}

float calculateImmediateSlope() {
  if (bufferSize < 2) return 0; // Ensure there are enough readings for calculation
  
  int latestIndex = (currentReadingIndex - 1 + bufferSize) % bufferSize;
  int previousIndex = (latestIndex - 1 + bufferSize) % bufferSize;
  
  float levelChange = readings[latestIndex].level - readings[previousIndex].level;
  long timeChange = readings[latestIndex].timestamp - readings[previousIndex].timestamp;
  
  if (timeChange == 0) return 0; // Avoid division by zero
  
  return levelChange / timeChange; // Returns the immediate slope
}

// float calculateAverageSlope() {
//   if (bufferSize < 2) return 0; // Need at least two readings to calculate slope
  
//   int firstIndex = currentReadingIndex; // The oldest reading in the buffer
//   int lastIndex = (currentReadingIndex - 1 + bufferSize) % bufferSize; // The newest reading in the buffer
  
//   float levelChange = readings[lastIndex].level - readings[firstIndex].level;
//   time_t timeChange = readings[lastIndex].timestamp - readings[firstIndex].timestamp;
  
//   if (timeChange == 0) return 0; // Prevent division by zero
  
//   return levelChange / timeChange; // Average slope over the buffer
// }

int interpolateWaterLevel(int raw) {
  if (raw <= lookupTable[0][0]) return lookupTable[0][1];
  if (raw >= lookupTable[lookupTableSize - 1][0]) return lookupTable[lookupTableSize - 1][1];

  for (int i = 0; i < lookupTableSize - 1; i++) {
    if (raw == lookupTable[i][0]) {
      return lookupTable[i][1];
    }
    if (raw > lookupTable[i][0] && raw < lookupTable[i + 1][0]) {
      float slope = (float)(lookupTable[i + 1][1] - lookupTable[i][1]) / (lookupTable[i + 1][0] - lookupTable[i][0]);
      float intercept = lookupTable[i][1] - slope * lookupTable[i][0];
      return slope * raw + intercept;
    }
  }
  return -1;
}

void updateBuffer(float level) {
  time_t now = time(nullptr);
  readings[currentReadingIndex].level = level;
  readings[currentReadingIndex].timestamp = now;
  currentReadingIndex = (currentReadingIndex + 1) % bufferSize; // Move to the next index, wrap around if at the end
}

void checkWaterLevel() {
  time_t now = time(nullptr);
  raw = analogRead(ADC_PIN);
  buf = ROD_LENGTH - interpolateWaterLevel(raw); // Convert raw to cm and invert
  updateBuffer(buf); // Update the circular buffer with the new reading

  Serial.print("Water Level: ");
  Serial.print(buf);
  Serial.print(" cm, raw: ");
  Serial.println(raw);
}

void activatePump() {
  digitalWrite(PUMP_RELAY_PIN, HIGH);
  Serial.println("Pump activated, flushing started...");
  
  while (true) { // Dedicated loop to monitor water level during flushing
    buf = interpolateWaterLevel(analogRead(ADC_PIN));
      
    if (buf <= minimumWaterLevel) {
      digitalWrite(PUMP_RELAY_PIN, LOW);
      Serial.print("Current water level: ");
      Serial.println(buf);
      Serial.println("Minimum water level reached. Pump deactivated.");
      break;
    }
    delay(100);
  }
}

void decideFlush() {
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);
  int hour = timeinfo->tm_hour;
  bool pump = false;
  bool publishSuccessfully;

  float immediateSlope = calculateImmediateSlope();

  // Simplified Day/Night Logic: Flush if water level or immediate slope is high
  if ((hour >= NIGHT || hour < MORNING) || buf > criticalWaterLevel || immediateSlope > SOME_THRESHOLD) {
    activatePump();
    pump = true;
  }

  String pumpStatus = pump ? "on" : "off";
  publishSuccessfully = mqttClient.publish("home/pumpStatus", pumpStatus.c_str());
  if (!publishSuccessfully) {
    Serial.println("Failed to publish the pump status.");
    // Implement retry logic here or simply log the failure
  }

  publishSuccessfully = mqttClient.publish("home/waterLevel", String(buf).c_str());
  if (!publishSuccessfully) {
    Serial.println("Failed to publish the water level.");
    // Implement retry logic here or simply log the failure
  }
}

void loop() {
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Attempting reconnection...");
    setup_wifi();
  } else {
    if (!mqttClient.connected()) {
      reconnect();
    }
    mqttClient.loop();
  }

  // Regardless of WiFi status, check water level and decide on flushing every SLEEP milliseconds
  if (now - lastCheck > SLEEP) {
    checkWaterLevel();
    decideFlush();
    lastCheck = now;
  }

  digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));
  delay(10); // Short delay to prevent watchdog timer reset without delaying sensor checks too much
}
