// ============================================================================
//  config.example.h  —  copy to config.h and fill in your values.
//  config.h is git-ignored so your WiFi password never ends up in the repo.
// ============================================================================
#pragma once

// ---- WiFi ------------------------------------------------------------------
// The ESP32 joins this network and serves the web UI on it. Put it close to a
// router/AP that actually reaches the battery compartment.
#define WIFI_SSID      "your-wifi-ssid"
#define WIFI_PASSWORD  "your-wifi-password"

// Hostname (reachable as http://<HOSTNAME>.local on the same network).
#define HOSTNAME       "lithink-bridge"

// ---- Batteries -------------------------------------------------------------
// One entry per JBD/Lithink battery: {MAC, display name}.
// Find the MAC with a BLE scanner (nRF Connect) or this sketch's serial log.
// Addresses are PUBLIC (static) on these BMS.
#define BATTERY_COUNT  2
#define BATTERIES { \
  {"aa:bb:cc:dd:ee:01", "Battery 1"}, \
  {"aa:bb:cc:dd:ee:02", "Battery 2"}, \
}

// ---- Control ---------------------------------------------------------------
// Charge/discharge MOSFET switching from the web UI. Leave 0 for a read-only
// monitor. Enabling it lets the browser turn a 300 A load on/off — use with
// care. The underlying JbdBms::setCharging()/setDischarging() exist regardless.
#define ENABLE_CONTROL 0

// ---- OTA (optional network-update module, see ota.h) -----------------------
// 1 = wireless firmware updates after the first USB flash (ArduinoOTA).
// Integrators who don't need it set this to 0 and the
// module is dropped entirely.
#define ENABLE_OTA   1
#define OTA_PASSWORD "change-me-ota"

// ---- Truma iNet X panel (optional module, see truma_ble.h) -----------------
// 1 = also connect to a Truma iNet X panel over BLE and show/control the
// heater in the web UI. Uses a third BLE connection next to the batteries.
// Pair once from the web UI ("Pair"), then choose "add device" on the panel.
// While the ESP is connected, the Truma app cannot connect to the panel.
// This module is GPL-3.0 (port of Moormerlander/esphome-truma-inetx).
#define ENABLE_TRUMA    0
// Name shown in the panel's device list.
#define TRUMA_USER_NAME "ESP32 Monitor"
