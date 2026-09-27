#pragma once
#include "Arduino.h"
#include "WiFiClient.h"
extern bool g_mqtt_connected;
struct PubSubClient {
  PubSubClient(WiFiClient&) {}
  void setServer(const char*, int) {} void setKeepAlive(int) {}
  void setBufferSize(int) {} void setCallback(void(*)(char*, byte*, unsigned int)) {}
  bool connected() { return g_mqtt_connected; }
  bool connect(const char*, const char*, const char*) { return true; }
  bool connect(const char*, const char*, const char*, const char*, int, bool, const char*) { return true; }
  void loop() {} void subscribe(const char*) {}
  bool publish(const char*, const char*) { return true; }
  bool publish(const char*, const char*, bool) { return true; }
};
