// -----------------------------------------------------------------------------
//  config.example.h
//
//  Copy this file to config.h and fill in your own values:
//
//      cp src/config.example.h src/config.h
//
//  config.h is listed in .gitignore and must never be committed.
// -----------------------------------------------------------------------------
#pragma once

// ---- Wi-Fi -------------------------------------------------------------------
#define WIFI_SSID       "your-wifi-ssid"
#define WIFI_PASSWORD   "your-wifi-password"

// ---- MQTT broker -------------------------------------------------------------
// Host or IP of the broker (e.g. the machine running Home Assistant).
#define MQTT_SERVER     "192.168.0.10"
#define MQTT_PORT       1883

// The broker MUST have a matching login. On the Home Assistant Mosquitto add-on
// these live under Settings > Add-ons > Mosquitto broker > Configuration:
//
//   logins:
//     - username: irrigator
//       password: <a long random password>
//
// If the firmware connects without credentials the broker rejects it with
// "received null username or password for unpwd check ... not authorised"
// and the zones silently never respond.
#define MQTT_USER       "irrigator"
#define MQTT_PASSWORD   "your-mqtt-password"
