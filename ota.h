// ============================================================================
//  ota.h — optional network firmware update (ArduinoOTA).
//
//  This is a *separate, optional module*. Build with ENABLE_OTA = 0 to leave it
//  out entirely (e.g. when integrating the battery reader into another product
//  that has its own update mechanism).
//
//  Bootstrap: the FIRST flash of this firmware must go over USB. After that,
//  ArduinoOTA accepts updates over WiFi (port 3232), so future flashes are
//  wireless:
//     PlatformIO : pio run -t upload --upload-port <hostname>.local
//     Arduino IDE: Tools -> Port -> network port "<hostname>"
//     espota     : espota.py -i <ip> -p 3232 -a <password> -f firmware.bin
//
//  ArduinoOTA.begin() also sets up mDNS (hostname + _arduino._tcp service).
// ============================================================================
#pragma once
#include <ArduinoOTA.h>
#include <WebServer.h>
#include <Update.h>

inline void otaBegin(const char *hostname, const char *password) {
  ArduinoOTA.setHostname(hostname);
  if (password && strlen(password) > 0) ArduinoOTA.setPassword(password);
  ArduinoOTA.onStart([]() { Serial.println("[ota] update start"); });
  ArduinoOTA.onEnd([]()   { Serial.println("[ota] update done"); });
  ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
    Serial.printf("[ota] %u%%\r", t ? (p * 100 / t) : 0);
  });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[ota] error %u\n", e); });
  ArduinoOTA.begin();  // starts mDNS + the arduino OTA service
}

inline void otaHandle() { ArduinoOTA.handle(); }

// ---------------------------------------------------------------------------
//  Browser-based firmware upload at /update (HTTP POST, multipart).
//
//  Unlike ArduinoOTA this uploads in the SAME direction as the web UI
//  (client -> ESP), so it works across routed / Tailscale / firewalled
//  networks where ArduinoOTA's reverse connection (ESP -> host) is blocked.
//
//  Protected with HTTP basic auth (user "admin", password = OTA_PASSWORD).
//    Browser : open http://<host>/update
//    curl    : curl -u admin:<pw> -F "fw=@firmware.bin" http://<host>/update
// ---------------------------------------------------------------------------
inline void otaWebHandlers(WebServer &server, const char *password) {
  // Upload form
  server.on("/update", HTTP_GET, [&server, password]() {
    if (!server.authenticate("admin", password)) return server.requestAuthentication();
    server.send(200, "text/html",
      "<!doctype html><meta name=viewport content='width=device-width'>"
      "<h3>Firmware-Update</h3>"
      "<form method='POST' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='fw' accept='.bin'> "
      "<input type='submit' value='Flash'></form>");
  });

  // Receive + flash. Two callbacks: the responder runs after the upload.
  // "OK" and the restart only follow an authorised upload that was written
  // completely; anything else (wrong password, broken upload) gets no reboot.
  static bool flashed = false;
  // WebServer also calls the upload callback for non-form POSTs ("raw" mode);
  // server.upload() is a null reference then. Keep Content-Type to tell them
  // apart (note: a later collectHeaders() call elsewhere would replace this).
  static const char *kHeaders[] = {"Content-Type"};
  server.collectHeaders(kHeaders, 1);
  server.on("/update", HTTP_POST,
    [&server, password]() {
      if (!server.authenticate("admin", password)) return server.requestAuthentication();
      const bool ok = flashed;
      flashed = false;
      server.send(ok ? 200 : 500, "text/plain", ok ? "OK - rebooting" : "FAILED");
      delay(400);
      if (ok) ESP.restart();
    },
    [&server, password]() {
      if (!server.header("Content-Type").startsWith("multipart/form-data")) return;
      HTTPUpload &up = server.upload();
      if (up.status == UPLOAD_FILE_START) {
        flashed = false;
        if (!server.authenticate("admin", password)) return;  // guard
        Serial.printf("[ota-web] receiving %s\n", up.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      } else if (up.status == UPLOAD_FILE_WRITE) {
        if (Update.isRunning()) Update.write(up.buf, up.currentSize);
      } else if (up.status == UPLOAD_FILE_END) {
        if (!Update.isRunning()) return;
        flashed = Update.end(true);
        if (flashed)
          Serial.printf("[ota-web] done, %u bytes\n", up.totalSize);
        else
          Update.printError(Serial);
      } else if (up.status == UPLOAD_FILE_ABORTED) {
        if (Update.isRunning()) Update.abort();
      }
    });
}
