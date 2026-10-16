// ============================================================================
//  lithink-ble-monitor — standalone ESP32 firmware (Arduino, no ESPHome)
//
//  Reads one or more JBD / Lithink LiFePO4 batteries over BLE and serves a
//  small web UI + JSON API over WiFi. The battery-reading core lives in
//  jbd_bms.h and is framework-free, so it can be lifted into another product.
//
//  Layout:
//    lithink-ble-monitor.ino  — WiFi, web server, task wiring (this file)
//    jbd_bms.h                — JBD BLE client + protocol parser (the reusable bit)
//    web_page.h               — the web UI (served from flash)
//    config.h                 — your WiFi + battery MACs (copy from config.example.h)
//
//  Libraries:  NimBLE-Arduino 1.4.x  (Arduino Library Manager / platformio.ini)
//  Board:      any classic ESP32 (esp32dev)
//
//  NOTE: built to spec but NOT verified on hardware by the author — test
//  against your real battery first.
// ============================================================================
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <NimBLEDevice.h>

#include "config.h"
#include "jbd_bms.h"
#include "web_page.h"
#if ENABLE_OTA
#include "ota.h"   // optional network-update module
#endif

struct BatteryCfg { const char *mac; const char *name; };
static BatteryCfg BAT_CFG[] = BATTERIES;

static JbdBms   *bms[BATTERY_COUNT];
static WebServer server(80);

// ---- JSON for the web UI ---------------------------------------------------
static String buildJson() {
  String s = "{\"batteries\":[";
  for (int i = 0; i < BATTERY_COUNT; i++) {
    JbdData &d = bms[i]->data;
    if (i) s += ",";
    s += "{\"name\":\"" + String(bms[i]->name()) + "\"";
    s += ",\"connected\":" + String(d.connected ? "true" : "false");
    s += ",\"valid\":" + String(d.valid ? "true" : "false");
    s += ",\"age_s\":" + String(d.valid ? (millis() - d.last_update_ms) / 1000 : 0);
    s += ",\"voltage\":" + String(d.total_voltage, 2);
    s += ",\"current\":" + String(d.current, 2);
    s += ",\"power\":" + String(d.power, 0);
    s += ",\"soc\":" + String(d.soc);
    s += ",\"remaining_ah\":" + String(d.capacity_remaining, 2);
    s += ",\"nominal_ah\":" + String(d.nominal_capacity, 2);
    s += ",\"cycles\":" + String(d.cycles);
    s += ",\"cell_count\":" + String(d.cell_count);
    s += ",\"cell_delta\":" + String(d.cell_delta, 3);
    s += ",\"cells\":[";
    for (int c = 0; c < d.cell_count && c < 32; c++) { if (c) s += ","; s += String(d.cell_v[c], 3); }
    s += "],\"temps\":[";
    for (int t = 0; t < d.temp_count && t < 6; t++) { if (t) s += ","; s += String(d.temp[t], 1); }
    s += "]";
    s += ",\"charging_fet\":" + String(d.charging_fet ? "true" : "false");
    s += ",\"discharging_fet\":" + String(d.discharging_fet ? "true" : "false");
    s += ",\"balancing\":" + String(d.balancing ? "true" : "false");
    s += ",\"protection\":" + String(d.protection);
    s += "}";
  }
  s += "]}";
  return s;
}

// ---- HTTP handlers ---------------------------------------------------------
static void handleRoot() {
  String page = FPSTR(INDEX_HTML);
  page.replace("__ENABLE_CONTROL__", String(ENABLE_CONTROL));
  server.send(200, "text/html; charset=utf-8", page);
}

static void handleData() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buildJson());
}

static void handleControl() {
#if ENABLE_CONTROL
  int  bat = server.arg("bat").toInt();
  String what = server.arg("what");
  bool on  = server.arg("on").toInt() != 0;
  if (bat < 0 || bat >= BATTERY_COUNT) { server.send(400, "text/plain", "bad bat"); return; }
  bool ok = (what == "charge")    ? bms[bat]->setCharging(on)
          : (what == "discharge") ? bms[bat]->setDischarging(on)
          : false;
  server.send(ok ? 200 : 500, "text/plain", ok ? "ok" : "fail");
#else
  server.send(403, "text/plain", "control disabled (build with ENABLE_CONTROL=1)");
#endif
}

// ---- BLE management task (keeps the web loop non-blocking) ------------------
static void bmsTask(void *) {
  for (;;) {
    for (int i = 0; i < BATTERY_COUNT; i++) bms[i]->loop();
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ---- Setup / loop ----------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[lithink-ble-monitor] boot");

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[wifi] connecting");
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(250); Serial.print("."); }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[wifi] connected, IP %s\n", WiFi.localIP().toString().c_str());
#if ENABLE_OTA
    otaBegin(HOSTNAME, OTA_PASSWORD);  // also starts mDNS (name + OTA service)
    Serial.println("[ota] ready for wireless updates");
#else
    MDNS.begin(HOSTNAME);
#endif
    MDNS.addService("http", "tcp", 80);
    Serial.printf("[mdns] http://%s.local\n", HOSTNAME);
  } else {
    Serial.println("[wifi] NOT connected (will keep retrying in background)");
  }

  // BLE: one shared controller, one client per battery.
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // max TX power helps through metal
  for (int i = 0; i < BATTERY_COUNT; i++) {
    bms[i] = new JbdBms(BAT_CFG[i].mac, BAT_CFG[i].name);
    bms[i]->begin();
  }
  xTaskCreatePinnedToCore(bmsTask, "bms", 8192, nullptr, 1, nullptr, 1);

  server.on("/", handleRoot);
  server.on("/api/data", handleData);
  server.on("/api/control", HTTP_POST, handleControl);
#if ENABLE_OTA
  otaWebHandlers(server, OTA_PASSWORD);  // browser/curl firmware upload at /update
#endif
  server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
  server.begin();
  Serial.println("[web] server started on :80");
}

void loop() {
#if ENABLE_OTA
  otaHandle();
#endif
  server.handleClient();
  delay(2);
}
