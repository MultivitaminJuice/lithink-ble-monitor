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
//    truma_ble.h + truma_core.* — optional Truma iNet X panel (ENABLE_TRUMA, GPL-3.0)
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
#include <esp_system.h>

#include "config.h"
#include "jbd_bms.h"
#include "web_page.h"
#if ENABLE_OTA
#include "ota.h"   // optional network-update module
#endif
#ifndef ENABLE_TRUMA
#define ENABLE_TRUMA 0
#endif
#ifndef TRUMA_USER_NAME
#define TRUMA_USER_NAME "ESP32 Monitor"
#endif
#if ENABLE_TRUMA
#include "truma_ble.h"   // optional Truma iNet X panel module
#endif

struct BatteryCfg { const char *mac; const char *name; };
static BatteryCfg BAT_CFG[] = BATTERIES;

static JbdBms   *bms[BATTERY_COUNT];
static WebServer server(80);
// NimBLE runs one connect at a time and cannot connect while scanning: the
// battery task and the Truma task take this lock around those operations.
static SemaphoreHandle_t bleGapLock;
#if ENABLE_TRUMA
static truma_ble::Panel truma;
#endif

// ---- JSON for the web UI ---------------------------------------------------
// JSON strings are reserved up front: growing them piece by piece fragments the
// heap until large blocks (web page, OTA) can no longer be allocated.
static String buildJson() {
  String s;
  s.reserve(256 + BATTERY_COUNT * 768);
  s += "{\"control\":" + String(ENABLE_CONTROL) + ",\"truma\":" + String(ENABLE_TRUMA) + ",\"batteries\":[";
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

#if ENABLE_TRUMA
// ---- JSON for the Truma card -----------------------------------------------
static String jsonStr(const char *v) {
  String s = "\"";
  for (const char *p = v; *p; p++) {
    if (*p == '"' || *p == '\\') { s += '\\'; s += *p; }
    else if ((uint8_t)*p < 0x20) s += ' ';
    else s += *p;
  }
  return s + "\"";
}
static String jsonNum(float v, int d) { return isnan(v) ? String("null") : String(v, d); }
static String jsonInt(int v) { return v < 0 ? String("null") : String(v); }

static String buildTrumaJson() {
  truma_ble::State t = truma.state();
  String s;
  s.reserve(3072);
  s += "{\"status\":" + jsonStr(t.status);
  s += ",\"session\":" + jsonStr(t.session);
  s += ",\"paired\":" + String(t.paired ? "true" : "false");
  s += ",\"pairing\":" + String(t.pairing ? "true" : "false");
  s += ",\"pairing_left_s\":" + String(t.pairing_left_s);
  s += ",\"connected\":" + String(t.connected ? "true" : "false");
  s += ",\"panel_name\":" + jsonStr(t.panel_name);
  s += ",\"last_disconnect\":" + jsonStr(t.last_disconnect);
  s += ",\"link_params\":" + jsonStr(t.link_params);
  s += ",\"error\":" + jsonStr(t.error);
  s += ",\"panel_error\":" + jsonStr(t.panel_error);
  s += ",\"line_power\":" + jsonInt(t.line_power);
  s += ",\"room_temp\":" + jsonNum(t.room_temp, 1);
  s += ",\"target_temp\":" + jsonNum(t.target_temp, 1);
  s += ",\"water_temp\":" + jsonNum(t.water_temp, 1);
  s += ",\"mode\":" + jsonInt(t.mode);
  s += ",\"heating_active\":" + jsonInt(t.heating_active);
  s += ",\"heating_mode\":" + jsonInt(t.heating_mode);
  s += ",\"water_active\":" + jsonInt(t.water_active);
  s += ",\"water_mode\":" + jsonInt(t.water_mode);
  s += ",\"diesel_level\":" + jsonInt(t.diesel_level);
  s += ",\"electric_level\":" + jsonInt(t.electric_level);
  s += ",\"fan_level\":" + jsonInt(t.fan_level);
  s += ",\"water_boost\":" + jsonInt(t.water_boost);
  s += ",\"water_boost_time\":" + jsonInt(t.water_boost_time);
  s += ",\"pwr_mode\":" + jsonInt(t.pwr_mode);
  s += ",\"brightness\":" + jsonInt(t.brightness);
  s += ",\"dark_brightness\":" + jsonInt(t.dark_brightness);
  s += ",\"display_timeout\":" + jsonInt(t.display_timeout);
  s += ",\"screensaver\":" + jsonInt(t.screensaver);
  s += ",\"panel_idle_s\":" + jsonInt(t.panel_idle_s);
  s += ",\"values_age_s\":" + String(t.values_age_s);
  s += ",\"restore_enabled\":" + String(t.restore_enabled ? "true" : "false");
  s += ",\"restored_age_s\":" + String(t.restored_age_s);
  s += ",\"devices\":[";
  for (int i = 0; i < t.device_count; i++) {
    const truma_ble::DeviceInfo &d = t.devices[i];
    if (i) s += ",";
    s += "{\"addr\":" + String(d.addr) + ",\"name\":" + jsonStr(d.name) + ",\"firmware\":" + jsonStr(d.firmware) +
         ",\"serial\":" + jsonStr(d.serial) + ",\"age_s\":" + String(d.age_s) + "}";
  }
  s += "],\"timers\":[";
  for (int i = 0; i < truma_ble::MAX_TIMERS; i++) {
    const truma_ble::TimerInfo &tm = t.timers[i];
    if (i) s += ",";
    s += "{\"defined\":" + String(tm.defined ? "true" : "false") + ",\"enabled\":" + jsonInt(tm.enabled) +
         ",\"name\":" + jsonStr(tm.name) + ",\"start\":" + jsonStr(tm.start) + ",\"end\":" + jsonStr(tm.end) +
         ",\"weekdays\":" + String(tm.weekdays) + ",\"symbol\":" + jsonInt(tm.symbol) + "}";
  }
  s += "]";
  s += "}";
  return s;
}

static void handleTrumaData() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buildTrumaJson());
}

static void handleTrumaBus() {
  server.sendHeader("Cache-Control", "no-store");
  truma.with_bus_text([](const char *text, size_t len) { server.send_P(200, "text/plain; charset=utf-8", text, len); });
}

static void handleTrumaLog() {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/plain; charset=utf-8", truma_ble::Panel::log_text());
}

// POST /api/truma?what=<action>&value=<n>
static void handleTrumaControl() {
  using namespace truma_inetx_proto;
  String what = server.arg("what");
  float  val  = server.arg("value").toFloat();
  int    iv   = server.arg("value").toInt();
  bool ok = false, valid = true;
  if      (what == "pair")        ok = truma.pair();
  else if (what == "unpair")      ok = truma.unpair();
  else if (what == "reconnect")   ok = truma.reconnect();
  else if (what == "mode")        { valid = iv >= 0 && iv <= 6;  ok = valid && truma.write("RoomClimate", "Mode", iv, DEV_PANEL); }
  else if (what == "target")      { valid = val >= 5 && val <= 30; ok = valid && truma.set_target(val); }
  else if (what == "heat_mode")   { valid = iv >= 0 && iv <= 1;  ok = valid && truma.write("AirHeating", "Mode", iv, DEV_HEATER); }
  else if (what == "water")       { valid = iv >= 0 && iv <= 1;  ok = valid && truma.write("WaterHeating", "Active", iv, DEV_HEATER); }
  else if (what == "water_mode")  { valid = iv >= 0 && iv <= 2;  ok = valid && truma.write("WaterHeating", "Mode", iv, DEV_HEATER); }
  else if (what == "diesel")      { valid = iv >= 0 && iv <= 1;  ok = valid && truma.write("EnergySrc", "DieselLevel", iv, 0); }
  else if (what == "electric")    { valid = iv >= 0 && iv <= 2;  ok = valid && truma.write("EnergySrc", "ElectricLevel", iv, 0); }
  else if (what == "fan")         { valid = iv >= 0 && iv <= 10; ok = valid && truma.write("AirCirculation", "FanLevel", iv, DEV_HEATER); }
  else if (what == "boost")       { valid = iv >= 0 && iv <= 1;  ok = valid && truma.write("WaterHeating", "FasterHeatingMode", iv, DEV_HEATER); }
  else if (what == "timer")       {
    int n = server.arg("n").toInt();
    valid = n >= 1 && n <= truma_ble::MAX_TIMERS && iv >= 0 && iv <= 1;
    char param[16];
    snprintf(param, sizeof(param), "Timer%dState", n);
    ok = valid && truma.write("TimerConfig", param, iv, DEV_PANEL);
  }
  // Panel display. PwrMode 0 (shutdown) is left out on purpose: it switches the
  // heater off too and the panel can only be switched on again on site.
  else if (what == "pwr_mode")    { valid = iv == 1 || iv == 3;  ok = valid && truma.write("PowerMgmt", "PwrMode", iv, DEV_PANEL); }  // 1 standby, 3 on
  else if (what == "brightness")  { valid = iv >= 10 && iv <= 100; ok = valid && truma.write("Panel", "Intst", iv, DEV_PANEL); }
  else if (what == "dark_brightness") { valid = iv >= 0 && iv <= 100; ok = valid && truma.write("Panel", "DarkIntst", iv, DEV_PANEL); }
  else if (what == "display_timeout") { valid = iv >= 10 && iv <= 600; ok = valid && truma.write("Panel", "DisplayTimeout", iv, DEV_PANEL); }  else if (what == "restore")     { valid = iv >= 0 && iv <= 1;  ok = valid && truma.set_restore(iv != 0); }
  else if (what == "error_reset") ok = truma.write("ErrorReset", "Req", 1, 0);
  else valid = false;
  if (!valid) { server.send(400, "text/plain", "bad request"); return; }
  server.send(ok ? 200 : 503, "text/plain", ok ? "ok" : "busy");
}
#endif

// ---- HTTP handlers ---------------------------------------------------------
// Sent straight from flash: a RAM copy of the page needs one large heap block.
// The page reads the build flags from /api/data.
static void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

// ---- Diagnostics + WiFi watchdog --------------------------------------------
static uint32_t wifiDrops = 0;       // times the WiFi link was seen down
static uint32_t wifiLostSince = 0;   // millis() when it went down, 0 = up
static uint32_t wifiLastRetry = 0;
static const uint32_t WIFI_RETRY_MS   = 60UL * 1000;      // reconnect after 1 min down
static const uint32_t WIFI_RESTART_MS = 5UL * 60 * 1000;  // restart after 5 min down

// Last WiFi outages, newest first (filled from the WiFi event task).
struct WifiDrop {
  uint32_t at_s;         // uptime when the link went down
  uint8_t  reason;       // first disconnect reason (wifi_err_reason_t)
  uint8_t  last_reason;  // reason of the latest failed reconnect attempt
  uint16_t attempts;     // disconnect events during this outage
  uint32_t down_s;       // outage length, 0 while still down
};
static const int WIFI_LOG = 5;
static WifiDrop wifiLog[WIFI_LOG];
static int wifiLogCount = 0;
static bool wifiUp = false;
static uint32_t wifiDownAt = 0;

static void onWifiEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    const uint8_t r = info.wifi_sta_disconnected.reason;
    if (wifiUp) {  // a new outage (ignore the attempts before the first connect)
      wifiUp = false;
      wifiDownAt = millis();
      for (int i = WIFI_LOG - 1; i > 0; i--) wifiLog[i] = wifiLog[i - 1];
      wifiLog[0] = {wifiDownAt / 1000, r, r, 1, 0};
      if (wifiLogCount < WIFI_LOG) wifiLogCount++;
    } else if (wifiLogCount > 0 && wifiLog[0].down_s == 0) {
      wifiLog[0].last_reason = r;
      wifiLog[0].attempts++;
    }
  } else if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    if (!wifiUp && wifiLogCount > 0 && wifiLog[0].down_s == 0)
      wifiLog[0].down_s = (millis() - wifiDownAt) / 1000 + 1;
    wifiUp = true;
  }
}

static const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}

// Keeps the device reachable: the ESP once stayed off the network for good.
static void wifiWatchdog() {
  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED) { wifiLostSince = 0; return; }
  if (wifiLostSince == 0) {
    wifiLostSince = now | 1;
    wifiLastRetry = now;
    wifiDrops++;
    Serial.println("[wifi] link down");
    return;
  }
  if (now - wifiLostSince > WIFI_RESTART_MS) {
    Serial.println("[wifi] down for 5 min, restarting");
    delay(100);
    ESP.restart();
  }
  if (now - wifiLastRetry > WIFI_RETRY_MS) {
    wifiLastRetry = now;
    Serial.println("[wifi] still down, reconnecting");
    WiFi.reconnect();
  }
}

static void handleStatus() {
  String s;
  s.reserve(1024);
  s += "{\"uptime_s\":" + String(millis() / 1000);
  s += ",\"reset_reason\":\"" + String(resetReason()) + "\"";
  s += ",\"free_heap\":" + String(ESP.getFreeHeap());
  s += ",\"min_free_heap\":" + String(ESP.getMinFreeHeap());
  s += ",\"max_alloc_heap\":" + String(ESP.getMaxAllocHeap());
  s += ",\"wifi_rssi\":" + String(WiFi.RSSI());
  s += ",\"wifi_drops\":" + String(wifiDrops);
  s += ",\"wifi_outages\":[";
  for (int i = 0; i < wifiLogCount; i++) {
    const WifiDrop d = wifiLog[i];
    if (i) s += ",";
    s += "{\"at_s\":" + String(d.at_s) + ",\"reason\":" + String(d.reason) + ",\"reason_name\":\"" +
         String(WiFi.disconnectReasonName((wifi_err_reason_t)d.reason)) + "\",\"last_reason\":" + String(d.last_reason) +
         ",\"attempts\":" + String(d.attempts) + ",\"down_s\":" + String(d.down_s) + "}";
  }
  s += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", s);
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
    for (int i = 0; i < BATTERY_COUNT; i++) {
      xSemaphoreTake(bleGapLock, portMAX_DELAY);  // loop() may connect (blocking)
      bms[i]->loop();
      xSemaphoreGive(bleGapLock);
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// ---- Setup / loop ----------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[lithink-ble-monitor] boot");

  WiFi.onEvent(onWifiEvent);
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

  // BLE: one shared controller, one client per battery (+ one for the panel).
  bleGapLock = xSemaphoreCreateMutex();
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // max TX power helps through metal
  for (int i = 0; i < BATTERY_COUNT; i++) {
    bms[i] = new JbdBms(BAT_CFG[i].mac, BAT_CFG[i].name);
    bms[i]->begin();
  }
  xTaskCreatePinnedToCore(bmsTask, "bms", 8192, nullptr, 1, nullptr, 1);
#if ENABLE_TRUMA
  truma.begin(TRUMA_USER_NAME, bleGapLock);  // starts its own task
#endif

  server.on("/", handleRoot);
  server.on("/api/data", handleData);
  server.on("/api/status", handleStatus);
  server.on("/api/control", HTTP_POST, handleControl);
#if ENABLE_TRUMA
  server.on("/api/truma", HTTP_GET, handleTrumaData);
  server.on("/api/truma", HTTP_POST, handleTrumaControl);
  server.on("/api/truma/log", HTTP_GET, handleTrumaLog);
  server.on("/api/truma/bus", HTTP_GET, handleTrumaBus);
#endif
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
  wifiWatchdog();
  delay(2);
}
