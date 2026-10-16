// ============================================================================
//  jbd_bms.h — JBD / Jiabaida LiFePO4 BMS client over BLE (NimBLE-Arduino)
//
//  Reusable, framework-free class that connects to a single JBD BMS, polls it,
//  parses the "basic info" (0x03) and "cell info" (0x04) frames, and exposes
//  the decoded values in a JbdData struct. Optional charge/discharge MOSFET
//  control via register 0xE1.
//
//  This is the piece meant to be lifted into another product: the BLE wiring is
//  thin, the protocol logic (parse + checksum + command framing) is plain C++.
//
//  GATT layout (standard JBD / "xiaoxiang"):
//    Service        0xFF00
//    Notify char    0xFF01   (BMS -> host, data frames)
//    Write char     0xFF02   (host -> BMS, commands)
//
//  Protocol (verified against syssi/esphome-jbd-bms):
//    Request  : DD A5 <reg> 00 <crc_hi> <crc_lo> 77
//    Response : DD <func> <status> <len> <data..> <crc_hi> <crc_lo> 77
//    Checksum : 16-bit, 0x0000 - sum(bytes[2 .. 2+len-1])   (status+len+data)
//
//  Built against NimBLE-Arduino 1.4.x. Author could NOT test on hardware —
//  verify against your real battery before relying on it.
// ============================================================================
#pragma once

#include <NimBLEDevice.h>
#include <Arduino.h>
#include <vector>

// ---- Protocol constants ----------------------------------------------------
static const uint16_t JBD_SERVICE_UUID   = 0xFF00;
static const uint16_t JBD_NOTIFY_UUID    = 0xFF01;
static const uint16_t JBD_WRITE_UUID     = 0xFF02;

static const uint8_t  JBD_PKT_START      = 0xDD;
static const uint8_t  JBD_PKT_END        = 0x77;
static const uint8_t  JBD_CMD_READ       = 0xA5;
static const uint8_t  JBD_CMD_WRITE      = 0x5A;
static const uint8_t  JBD_REG_BASICINFO  = 0x03;
static const uint8_t  JBD_REG_CELLINFO   = 0x04;
static const uint8_t  JBD_REG_MOS        = 0xE1;  // charge/discharge MOSFET control

// ---- Decoded battery data --------------------------------------------------
struct JbdData {
  bool     connected        = false;
  bool     valid            = false;  // at least one full basic-info frame parsed
  uint32_t last_update_ms   = 0;

  float    total_voltage    = 0;  // V
  float    current          = 0;  // A (signed: + charge, - discharge)
  float    power            = 0;  // W
  uint8_t  soc              = 0;  // %
  float    capacity_remaining = 0;  // Ah
  float    nominal_capacity   = 0;  // Ah
  uint16_t cycles           = 0;

  uint8_t  cell_count       = 0;
  float    cell_v[32]       = {0};  // V per cell
  float    cell_min         = 0;    // V
  float    cell_max         = 0;    // V
  float    cell_delta       = 0;    // V

  uint8_t  temp_count       = 0;
  float    temp[6]          = {0};  // °C

  bool     charging_fet     = false;  // charge MOSFET enabled
  bool     discharging_fet  = false;  // discharge MOSFET enabled
  bool     balancing        = false;
  uint16_t protection       = 0;      // raw protection bitmask (0 = OK)
};

// ---- One BMS connection ----------------------------------------------------
class JbdBms : public NimBLEClientCallbacks {
 public:
  JbdBms(const char *mac, const char *name) : mac_(mac), name_(name) {}

  const char *name() const { return name_; }
  const char *mac()  const { return mac_;  }
  JbdData     data;  // latest decoded values (read from the web layer)

  // Call once from setup() AFTER NimBLEDevice::init().
  void begin() {
    client_ = NimBLEDevice::createClient(NimBLEAddress(mac_, BLE_ADDR_PUBLIC));
    client_->setClientCallbacks(this, false);
    // NOTE: NimBLE's setConnectTimeout() unit (s vs ms) differs across versions,
    // so we leave the default. A failed connect blocks only this BLE manager
    // task (not the web server). Tune per your NimBLE version if desired.
  }

  // Drive the connection + polling. Call repeatedly from a dedicated task.
  void loop() {
    uint32_t now = millis();

    if (!client_->isConnected()) {
      data.connected = false;
      if (now - last_attempt_ < RECONNECT_INTERVAL_MS) return;
      last_attempt_ = now;
      connect_();
      return;
    }

    // Connected: poll on an interval. We request basic info; the notify handler
    // chains the cell-info request once basic info arrives.
    data.connected = true;
    if (now - last_poll_ >= POLL_INTERVAL_MS) {
      last_poll_ = now;
      requestBasicInfo_();
    }
  }

  // ---- Optional control (300 A BMS — use deliberately!) -------------------
  bool setCharging(bool on)    { return changeMosfet_(0, on); }  // bit 0
  bool setDischarging(bool on) { return changeMosfet_(1, on); }  // bit 1

  // NimBLEClientCallbacks
  void onDisconnect(NimBLEClient *) override { data.connected = false; }

 private:
  // ---- connection ---------------------------------------------------------
  void connect_() {
    if (!client_->connect()) return;  // connect to the stored address
    auto *svc = client_->getService(JBD_SERVICE_UUID);
    if (!svc) { client_->disconnect(); return; }
    notify_ = svc->getCharacteristic(JBD_NOTIFY_UUID);
    write_  = svc->getCharacteristic(JBD_WRITE_UUID);
    if (!notify_ || !write_) { client_->disconnect(); return; }

    notify_->subscribe(true, [this](NimBLERemoteCharacteristic *, uint8_t *d, size_t len, bool) {
      this->onNotify_(d, len);
    });
    buffer_.clear();
    last_poll_ = 0;  // poll immediately
  }

  // ---- command framing ----------------------------------------------------
  // 16-bit checksum over the given bytes: 0x0000 - sum(bytes)
  static uint16_t chksum(const uint8_t *d, uint16_t len) {
    uint16_t cs = 0;
    for (uint16_t i = 0; i < len; i++) cs = (uint16_t)(cs - d[i]);
    return cs;
  }

  // Build and send: DD <cmd> <reg> <len> <data..> <crc_hi> <crc_lo> 77
  bool sendCommand_(uint8_t cmd, uint8_t reg, const uint8_t *payload = nullptr, uint8_t plen = 0) {
    if (!write_) return false;
    std::vector<uint8_t> f(plen + 7);
    f[0] = JBD_PKT_START;
    f[1] = cmd;
    f[2] = reg;
    f[3] = plen;
    for (uint8_t i = 0; i < plen; i++) f[4 + i] = payload[i];
    uint16_t crc = chksum(&f[2], plen + 2);  // over reg + len + data
    f[4 + plen] = (uint8_t)(crc >> 8);
    f[5 + plen] = (uint8_t)(crc & 0xFF);
    f[6 + plen] = JBD_PKT_END;
    return write_->writeValue(f.data(), f.size(), false);  // no response
  }

  void requestBasicInfo_() { sendCommand_(JBD_CMD_READ, JBD_REG_BASICINFO); }
  void requestCellInfo_()  { sendCommand_(JBD_CMD_READ, JBD_REG_CELLINFO);  }

  bool writeRegister_(uint8_t reg, uint16_t value) {
    uint8_t payload[2] = {(uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
    return sendCommand_(JBD_CMD_WRITE, reg, payload, 2);
  }

  // Replicates syssi's change_mosfet_status(): bit 0 = charge, 1 = discharge.
  // Register value is inverted (1 = off/closed, 0 = on/open).
  bool changeMosfet_(uint8_t bit, bool on) {
    if (mosfet_raw_ == 0xFF) return false;  // unknown yet (need a basic frame first)
    uint16_t v = (uint16_t)((mosfet_raw_ & ~(1 << bit)) | ((uint8_t)on << bit));
    mosfet_raw_ = (uint8_t)v;
    v &= 0x03;
    v ^= (1 << 0);
    v ^= (1 << 1);
    return writeRegister_(JBD_REG_MOS, v);
  }

  // ---- frame assembly + dispatch -----------------------------------------
  void onNotify_(const uint8_t *d, size_t len) {
    // A new frame starts with 0xDD and carries status 0x00 at index 2.
    if (len >= 3 && d[0] == JBD_PKT_START && d[2] == 0x00) buffer_.clear();
    buffer_.insert(buffer_.end(), d, d + len);

    if (buffer_.size() < 7 || buffer_[0] != JBD_PKT_START) return;

    uint8_t  function = buffer_[1];
    uint8_t  status   = buffer_[2];
    uint8_t  data_len = buffer_[3];
    size_t   frame_len = 4 + data_len + 3;
    if (buffer_.size() < frame_len) return;  // wait for the rest

    bool ok = buffer_.back() == JBD_PKT_END && status == 0x00;
    if (ok) {
      uint16_t crc_calc = chksum(&buffer_[2], data_len + 2);
      uint16_t crc_recv = (uint16_t)(buffer_[frame_len - 3] << 8) | buffer_[frame_len - 2];
      ok = (crc_calc == crc_recv);
    }
    if (ok) {
      const uint8_t *payload = &buffer_[4];
      if (function == JBD_REG_BASICINFO) {
        parseBasicInfo_(payload, data_len);
        requestCellInfo_();  // chain the cell-voltage request
      } else if (function == JBD_REG_CELLINFO) {
        parseCellInfo_(payload, data_len);
      }
    }
    buffer_.clear();
  }

  static uint16_t be16(const uint8_t *p, size_t i) { return (uint16_t)(p[i] << 8) | p[i + 1]; }

  void parseBasicInfo_(const uint8_t *p, uint8_t len) {
    if (len < 23) return;
    // bit 7 of byte 20 selects the capacity/current unit scale (0.1 vs 0.01)
    float scale = (p[20] & 0x80) ? 0.1f : 0.01f;

    data.total_voltage     = be16(p, 0) * 0.01f;
    data.current           = (int16_t)be16(p, 2) * scale;
    data.power             = data.total_voltage * data.current;
    data.capacity_remaining = be16(p, 4) * scale;
    data.nominal_capacity   = be16(p, 6) * scale;
    data.cycles            = be16(p, 8);

    uint32_t balance = ((uint32_t)be16(p, 14) << 16) | be16(p, 12);
    data.balancing     = balance > 0;
    data.protection    = be16(p, 16);
    data.soc           = p[19];

    mosfet_raw_        = p[20];
    data.charging_fet    = p[20] & 0x01;
    data.discharging_fet = p[20] & 0x02;

    data.cell_count    = p[21];
    uint8_t ntc_avail  = (len > 23) ? (len - 23) / 2 : 0;
    data.temp_count    = min((uint8_t)min(p[22], (uint8_t)6), ntc_avail);
    for (uint8_t i = 0; i < data.temp_count; i++)
      data.temp[i] = (be16(p, 23 + i * 2) - 2731) * 0.1f;  // 0.1 K -> °C

    data.valid = true;
    data.last_update_ms = millis();
  }

  void parseCellInfo_(const uint8_t *p, uint8_t len) {
    if (len < 2 || (len % 2) != 0) return;
    uint8_t cells = min((uint8_t)(len / 2), (uint8_t)32);
    float mn = 100, mx = -100;
    for (uint8_t i = 0; i < cells; i++) {
      float v = be16(p, i * 2) * 0.001f;
      data.cell_v[i] = v;
      if (v < mn) mn = v;
      if (v > mx) mx = v;
    }
    if (cells) { data.cell_min = mn; data.cell_max = mx; data.cell_delta = mx - mn; }
    data.last_update_ms = millis();
  }

  // ---- state --------------------------------------------------------------
  static const uint32_t POLL_INTERVAL_MS      = 10000;
  static const uint32_t RECONNECT_INTERVAL_MS = 5000;

  const char          *mac_;
  const char          *name_;
  NimBLEClient        *client_  = nullptr;
  NimBLERemoteCharacteristic *notify_ = nullptr;
  NimBLERemoteCharacteristic *write_  = nullptr;
  std::vector<uint8_t> buffer_;
  uint32_t             last_attempt_ = 0;
  uint32_t             last_poll_    = 0;
  uint8_t              mosfet_raw_   = 0xFF;  // 0xFF = unknown
};
