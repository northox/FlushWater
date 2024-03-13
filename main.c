#include <ESP8266WiFi.h> #include <ESP8266HTTPClient.h> #include <time.h>
#define max 986 #define min 188 #define rodLength 42 #define aPin A0 #define relayPin 2
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";
const char* serverName = "http://your-server.com/post-data";
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0; // Adjust according to your timezone, in seconds
const int daylightOffset_sec = 3600; // Adjust if your location has Daylight Saving Time
const int variation = max - min;
const int cmSplit = variation / rodLength; int raw = 0;
int buf = 0;
const int waterLevelThreshold = 30; // Threshold for activating the relay
const int criticalWaterLevel = 40; // Critical level to force pump activation regardless of time const int minimumWaterLevel = 10; // Minimum water level to prevent the pump from running dry

 void setup() { pinMode(LED_BUILTIN, OUTPUT); pinMode(relayPin, OUTPUT); digitalWrite(relayPin, LOW);
Serial.begin(9600); WiFi.begin(ssid, password);
while (WiFi.status() != WL_CONNECTED) { delay(500);
Serial.print(".");
}
Serial.println("\nConnected to WiFi"); configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
// Wait for time to be set
while (time(nullptr) < 24 * 3600) {
Serial.print(".");
delay(1000); }
Serial.println("\nTime set"); }
void loop() {
time_t now = time(nullptr);
struct tm* timeinfo = localtime(&now);
digitalWrite(LED_BUILTIN, HIGH); delay(500);
raw = analogRead(aPin); if (raw) {
buf = raw - min; buf = buf / cmSplit;
Serial.print("Water Level: "); Serial.print(buf); Serial.println(" cm");
// Determine if it's night or day
bool isNight = timeinfo->tm_hour < 6 || timeinfo->tm_hour >= 20;

 // Turn off the pump if water level is below the minimum if (buf < minimumWaterLevel) {
digitalWrite(relayPin, LOW); }
// Otherwise, handle pump activation based on time and water level thresholds else if (isNight || buf >= criticalWaterLevel) {
digitalWrite(relayPin, HIGH);
} else if (buf >= waterLevelThreshold && buf < criticalWaterLevel) {
// Optionally, add conditions for daytime operation if needed
digitalWrite(relayPin, LOW); // Example: turn off the pump unless it's critical }
if (WiFi.status() == WL_CONNECTED) {
HTTPClient http;
http.begin(serverName);
http.addHeader("Content-Type", "application/x-www-form-urlencoded");
String httpRequestData = "level=" + String(buf);
int httpResponseCode = http.POST(httpRequestData);
Serial.print("HTTP Response code: "); Serial.println(httpResponseCode);
http.end(); }
digitalWrite(LED_BUILTIN, LOW);
delay(5000); } else {
delay(1000); }
}
