#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ezTime.h>
#include <Ticker.h>

#define RELAY_PIN 5  // Adjust to your actual relay control pin

const char* ssid = "x";
const char* password = "y";
const char* mqttServer = "z";
const int mqttPort = 1883;
const char* ntpServer = "pool.ntp.org";

WiFiClient espClient;
PubSubClient mqttClient(espClient);
Timezone myTZ;
Ticker watchdog;
Ticker wifiReconnectTicker;

unsigned long downtimeTimer = 0;
unsigned long compensationTime = 0;
unsigned long lastCommandTime = 0;
unsigned long lastPumpMillis = 0;

void watchdogReset() {
  ESP.restart();
}

void checkWiFiConnection() {
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.reconnect();
    } else {
        Serial.println("WiFi connected");
    }
}

void setup() {
    Serial.begin(9600);
    pinMode(RELAY_PIN, OUTPUT);
    digitalWrite(RELAY_PIN, HIGH); // Set pump to active by default

    watchdog.attach(300, watchdogReset);

    WiFi.begin(ssid, password);
    wifiReconnectTicker.attach(30, checkWiFiConnection); // Check WiFi connection every 30 seconds

    mqttClient.setServer(mqttServer, mqttPort);
    mqttClient.setCallback(mqttCallback);

    setServer(ntpServer); // Set NTP server
    waitForSync(); // Synchronize time with NTP server

    myTZ.setLocation(F("America/New_York")); // Set to your correct timezone
    printTime(); // Print local time and UTC time after NTP synchronization
    
    connectToMQTT();
}

void loop() {
    watchdog.detach();
    watchdog.attach(300, watchdogReset);

    if (!mqttClient.connected()) {
        connectToMQTT();
    }
    mqttClient.loop();

    if (timeStatus() != timeSet) {
      syncNTPTime();
    }
    events(); // Handle ezTime events

    unsigned long currentMillis = millis();
    managePump(currentMillis);

    delay(500);
}

void connectToMQTT() {
    while (!mqttClient.connected()) {
        if (mqttClient.connect("ESP8266Client")) {
            mqttClient.subscribe("pump/control");
        }
        delay(500);
    }
    Serial.println("Connected to MQTT and subscribed.");
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message = "";
    for (int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    if (String(topic) == "pump/control") {
        if (message == "stop" && (millis() - lastCommandTime < downtimeTimer)) {
            downtimeTimer += 1800000;  // Add 30 minutes
        } else {
            downtimeTimer = 1800000;  // Set new 30 minutes
            lastCommandTime = millis();
        }
        Serial.println("Downtime timer updated");
    }
}

void managePump(unsigned long currentMillis) {
    if (myTZ.hour() >= 22 || myTZ.hour() < 6) {
        if (compensationTime > 0) {
            digitalWrite(RELAY_PIN, HIGH);
            compensationTime -= min(compensationTime, (currentMillis - lastPumpMillis));
        } else {
            digitalWrite(RELAY_PIN, LOW); // Only turn off at night if no compensation is needed
        }
    } else {
        if (downtimeTimer > 0 && (currentMillis - lastCommandTime < downtimeTimer)) {
            digitalWrite(RELAY_PIN, LOW);
        } else {
            digitalWrite(RELAY_PIN, HIGH); // Default to ON during daytime
            compensationTime += downtimeTimer;
            downtimeTimer = 0;
        }
    }
    lastPumpMillis = currentMillis;
}

void syncNTPTime() {
    waitForSync();
    printTime();
}

void printTime() {
    Serial.println("Local Time: " + myTZ.dateTime("Y-m-d H:i:s"));
    Serial.println("UTC Time: " + UTC.dateTime("Y-m-d H:i:s"));
}
