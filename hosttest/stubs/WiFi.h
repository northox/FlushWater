#pragma once
#include "Arduino.h"
#define WL_CONNECTED 3
#define WIFI_STA 1
extern int g_wifi_status;
struct IPAddr { String toString() const { return String("10.0.0.1"); } };
struct WiFiClass {
  void mode(int) {} void persistent(bool) {} void setAutoReconnect(bool) {}
  void setSleep(bool) {} void begin(const char*, const char*) {}
  int  status() { return g_wifi_status; }
  void disconnect(bool = false) {}
  IPAddr localIP() { return IPAddr(); }
  int  RSSI() { return -60; }
};
extern WiFiClass WiFi;
