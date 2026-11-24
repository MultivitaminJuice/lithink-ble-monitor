// ============================================================================
//  truma_ble.h — Truma iNet X panel over BLE (NimBLE-Arduino glue)
//
//  Connects the ESP32 to a Truma iNet X panel the way the Truma app does, so
//  the heater can be read and controlled from the web UI. The protocol
//  (transport framing, session, CBOR) lives unchanged in truma_core.{h,cpp};
//  this file is only the BLE glue: scanning, pairing/bonding, reconnecting
//  and GATT I/O.
//
//  Port of the ESPHome/Bluedroid glue of Moormerlander/esphome-truma-inetx
//  v0.7.6 to NimBLE-Arduino 1.4.x. Licensed GPL-3.0 like the original
//  (see LICENSE-GPL-3.0). Panel rules carried over from it:
//    * encrypt BEFORE enabling notifications (a CCCD write that races ahead
//      of encryption fails and the panel drops the link)
//    * never drop a working link on purpose: after a closed connection the
//      panel can refuse new ones until it is power-cycled, so session
//      problems are handled by restarting the session over the same link
//    * the panel rotates its address (RPA); its bond is found again via the
//      IRK. On the classic ESP32 NimBLE resolves peer RPAs in the host
//      (BLE_HOST_BASED_PRIVACY), like Bluedroid does there.
//    * only one app connection: the Truma app cannot connect while we do
//
//  Threading: everything below runs in one FreeRTOS task (begin() starts
//  it). NimBLE callbacks (host task) only queue data; the web layer reads a
//  snapshot (state()) and posts commands (pair(), write(), ...).
//
//  All bonds in the NimBLE store are treated as panel bonds: the JBD
//  batteries connect without pairing and never create one.
// ============================================================================
#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <mbedtls/aes.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <cmath>
#include <cstdarg>
#include <new>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <strings.h>
#include <vector>

#include "nimble/nimble/host/include/host/ble_hs.h"
#include "truma_core.h"

namespace truma_ble {

namespace proto = truma_inetx_proto;

// ---- Log: Serial + a small ring buffer the web UI can show ------------------
static const int LOG_LINES = 50;
static const int LOG_LEN = 128;
static char g_log[LOG_LINES][LOG_LEN];
static int g_log_head = 0;
static int g_log_count = 0;
static portMUX_TYPE g_log_mux = portMUX_INITIALIZER_UNLOCKED;

// level: 0 error, 1 warning, 2 info, 3 debug, 4 verbose (same as truma_core)
static void log_line(int level, const char *msg) {
  if (level >= 4) return;
  static const char LETTER[] = "EWID";
  char line[LOG_LEN];
  snprintf(line, sizeof(line), "%6lus %c %s", (unsigned long)(millis() / 1000),
           LETTER[level < 0 ? 0 : level], msg);
  Serial.printf("[truma] %s\n", line);
  if (level >= 3) return;  // debug goes to Serial only
  portENTER_CRITICAL(&g_log_mux);
  memcpy(g_log[g_log_head], line, LOG_LEN);
  g_log_head = (g_log_head + 1) % LOG_LINES;
  if (g_log_count < LOG_LINES) g_log_count++;
  portEXIT_CRITICAL(&g_log_mux);
}

static void logf(int level, const char *fmt, ...) {
  char buf[LOG_LEN];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  log_line(level, buf);
}

// ---- Helpers ----------------------------------------------------------------
static void aes128_ecb(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
  mbedtls_aes_free(&ctx);
}

// NimBLE keeps addresses LSB first; truma_core and the logs use MSB first.
static void to_msb(const NimBLEAddress &a, uint8_t out[6]) {
  const uint8_t *n = a.getNative();
  for (int i = 0; i < 6; i++) out[i] = n[5 - i];
}

static uint64_t addr_key(const uint8_t a[6]) {
  uint64_t k = 0;
  for (int i = 0; i < 6; i++) k = (k << 8) | a[i];
  return k;
}

static std::string addr_str(const uint8_t a[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
  return buf;
}

static std::string uuid_str(const uint8_t b[16], bool upper) {
  char buf[37];
  snprintf(buf, sizeof(buf),
           upper ? "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X"
                 : "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
  return buf;
}

// ---- Snapshot for the web layer -------------------------------------------------
// Numbers are NAN / -1 while unknown.
struct DeviceInfo {                 // one device on the Truma bus
  uint16_t addr = 0;
  char name[32] = "";               // Identify.Name
  char firmware[16] = "";           // Identify.SwMaj.SwMin.SwBgFx
  char serial[32] = "";             // Identify.SerialNr
  uint32_t age_s = 0;               // since it last sent anything
};
struct TimerInfo {                  // TimerConfig.TimerN on the panel
  bool defined = false;             // id != 0
  int enabled = -1;                 // TimerConfig.TimerNState: 0 off, 1 on
  char name[24] = "";
  char start[6] = "";               // "HH:MM"
  char end[6] = "";
  uint8_t weekdays = 0;             // bit 0 = Monday ... bit 6 = Sunday
  int symbol = -1;                  // 0 auto, 1 heating, 2 heating AC, 3 cooling, 4 fan, 5 dehumid, 6 water
};
static const int MAX_DEVICES = 6;
static const int MAX_TIMERS = 6;

struct State {
  const char *status = "starting";  // machine-readable code, translated by the web UI
  const char *session = "idle";     // truma_core session state name
  bool paired = false;
  bool pairing = false;
  bool connected = false;           // session running, values are live
  uint16_t pairing_left_s = 0;
  char panel_name[32] = "";
  char last_disconnect[128] = "";
  char link_params[64] = "";
  char error[128] = "";             // heater ErrorReset.ErrCode as text ("[]" = none)
  char panel_error[128] = "";       // panel ErrorReset.ErrCode as text
  float room_temp = NAN;            // °C
  float target_temp = NAN;          // °C, set point of the running mode
  float water_temp = NAN;           // °C
  int mode = -1;                    // RoomClimate.Mode: 0 off, 1 auto, 2 cool, 3/4 heat, 5 fan, 6 dry
  int heating_active = -1;          // AirHeating.Active
  int heating_mode = -1;            // AirHeating.Mode
  int water_active = -1;            // WaterHeating.Active
  int water_mode = -1;              // WaterHeating.Mode: 0 eco, 1 comfort, 2 hot
  int diesel_level = -1;            // EnergySrc.DieselLevel: 0 off, 1 on
  int electric_level = -1;          // EnergySrc.ElectricLevel: 0 off, 1 = 900 W, 2 = 1800 W
  int line_power = -1;              // LinePower.Plugged: 1 = 230 V connected
  int fan_level = -1;               // AirCirculation.FanLevel: 0-10
  int water_boost = -1;             // WaterHeating.FasterHeatingMode: 0 off, 1 on
  int water_boost_time = -1;        // WaterHeating.FasterHeatingModeTime: seconds left
  int pwr_mode = -1;                // panel PowerMgmt.PwrMode: 0 off, 1 standby, 3 on, 4/5 going off/standby
  int brightness = -1;              // Panel.Intst: %
  int dark_brightness = -1;         // Panel.DarkIntst: 1-10 while dimmed (the panel ignores other values)
  int display_timeout = -1;         // Panel.DisplayTimeout: s
  int screensaver = -1;             // Panel.Screensaver: 1 = display dimmed right now (state, not a setting)
  int32_t panel_idle_s = -1;        // Panel.UserInactiveSince: s since the last real use (taps on the lock screen don't count)
  int32_t values_age_s = -1;        // since the session last ran (0 = live, -1 = never)
  bool restore_enabled = true;      // restore settings after a panel reset
  int32_t restored_age_s = -1;      // since the last restore, -1 = none
  bool frost_enabled = true;        // frost guard: heat to 5 °C when the room is colder
  int32_t frost_age_s = -1;         // since the frost guard last wrote, -1 = never
  uint8_t device_count = 0;
  DeviceInfo devices[MAX_DEVICES];
  TimerInfo timers[MAX_TIMERS];
};

class Panel : public proto::TransportIO, public NimBLEClientCallbacks {
 public:
  Panel() : transport_(this), session_(&transport_) {}

  // Call once from setup() AFTER NimBLEDevice::init(). gap_lock serialises
  // scanning/connecting with the battery task (NimBLE runs one connect at a
  // time and cannot connect while scanning).
  bool begin(const char *user_name, SemaphoreHandle_t gap_lock) {
    proto::set_log_fn(log_line);
    gap_lock_ = gap_lock;
    cmd_q_ = xQueueCreate(8, sizeof(Command));
    rx_q_ = xQueueCreate(24, sizeof(Rx));
    state_mutex_ = xSemaphoreCreateMutex();

    load_identity_(user_name);
    session_.on_update = [this](uint16_t, const std::string &, const std::string &, const proto::CborValue &) {
      last_update_ms_ = millis();
      values_dirty_ = true;
    };
    session_.on_ready = [this]() {
      failures_ = 0;
      auth_failures_ = 0;
      logf(2, "Connected to the Truma panel (bus address 0x%04X)", session_.assigned_addr());
      load_bonds_();  // a fresh bond is surely persisted by now
      if (bonds_.empty()) logf(1, "No bond stored for the panel: it will not be found again after a link loss");
      conn_params_at_ = millis() | 1;  // after the session loop, not inside the callback
      restore_check_at_ = (millis() + RESTORE_SETTLE_MS) | 1;  // let the values arrive first
      frost_check_at_ = (millis() + FROST_FIRST_MS) | 1;
    };
    session_.on_failed = [this](const char *reason) { soft_restart_(reason); };
    transport_.on_fatal = [this](const char *reason) { soft_restart_(reason); };

    // Just Works bonding (the panel has no code dialogue) and ask for the
    // panel's identity key (IRK) so its rotating address can be recognised.
    // Keyboard/display + MITM are still offered: the setting the long-term
    // tests of the original ran with.
    NimBLEDevice::setSecurityAuth(true, true, true);  // bond, MITM, secure connections
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_DISPLAY);
    NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
    NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);

    client_ = NimBLEDevice::createClient();
    if (client_ == nullptr) {
      logf(0, "No free BLE client slot for the panel (raise CONFIG_BT_NIMBLE_MAX_CONNECTIONS)");
      return false;
    }
    client_->setClientCallbacks(this, false);
    // onDisconnect() does not get the HCI reason in NimBLE-Arduino 1.4.x; a
    // GAP listener sees the event first, while the client still has its handle.
    ble_gap_event_listener_register(&gap_listener_, [](ble_gap_event *e, void *arg) -> int {
      auto *self = static_cast<Panel *>(arg);
      if (e->type == BLE_GAP_EVENT_DISCONNECT && self->client_ != nullptr &&
          e->disconnect.conn.conn_handle == self->client_->getConnId())
        self->disc_reason_ = e->disconnect.reason;
      return 0;
    }, this);
    client_->setConnectTimeout(10);  // seconds in NimBLE-Arduino 1.4.x
    // 8.75-11.25 ms like ESPHome: the panel rejects longer intervals.
    client_->setConnectionParams(7, 9, 0, 400);

    load_bonds_();
    if (bonds_.empty())
      logf(1, "Not paired with a Truma panel yet: press 'Pair', then put the panel into add-device mode");
    else
      logf(2, "Paired with the panel '%s'; waiting for its advertisement", panel_name_.c_str());
    set_link_(Link::WAIT_ADV);
    xTaskCreatePinnedToCore(task_, "truma", 10240, this, 1, nullptr, 1);
    return true;
  }

  // ---- Web layer API (any task) -----------------------------------------------
  State state() {
    State s;
    if (state_mutex_ && xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(200)) == pdTRUE) {
      s = snapshot_;
      xSemaphoreGive(state_mutex_);
    }
    return s;
  }
  bool pair() { return post_(Command::PAIR); }
  bool unpair() { return post_(Command::UNPAIR); }
  bool reconnect() { return post_(Command::RECONNECT); }
  bool set_restore(bool on) { return post_(Command::RESTORE, "", "", on ? 1 : 0, 0); }
  bool set_frost(bool on) { return post_(Command::FROST, "", "", on ? 1 : 0, 0); }  // Write an integer parameter; dest 0 = whichever bus device publishes it.
  bool write(const char *topic, const char *param, int32_t value, uint16_t dest) {
    return post_(Command::WRITE, topic, param, value, dest);
  }
  // Set point of the running mode (°C).
  bool set_target(float celsius) {
    return post_(Command::TARGET, "", "", (int32_t)lroundf(celsius * 10.0f), 0);
  }

  // Every value the session knows, one "device topic.param = value" per line
  // (diagnostics). Built by the Truma task; waits up to 1 s for it.
  // The text is handed to `send` without a copy and freed afterwards, so the
  // few KB are only held for the request (the heap is tight while connected).
  template <typename F> void with_bus_text(F send) {
    const uint32_t seq = dump_seq_;
    if (!post_(Command::DUMP)) { send("busy\n", 5); return; }
    for (int i = 0; i < 50 && dump_seq_ == seq; i++) vTaskDelay(pdMS_TO_TICKS(20));
    if (xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(200)) != pdTRUE) { send("busy\n", 5); return; }
    send(bus_dump_.c_str(), bus_dump_.size());
    std::string().swap(bus_dump_);
    xSemaphoreGive(state_mutex_);
  }

  static String log_text() {
    String out;
    portENTER_CRITICAL(&g_log_mux);
    int n = g_log_count;
    int start = (g_log_head - n + LOG_LINES) % LOG_LINES;
    static char copy[LOG_LINES][LOG_LEN];
    for (int i = 0; i < n; i++) memcpy(copy[i], g_log[(start + i) % LOG_LINES], LOG_LEN);
    portEXIT_CRITICAL(&g_log_mux);
    for (int i = 0; i < n; i++) { out += copy[i]; out += '\n'; }
    return out;
  }

  // ---- truma_inetx_proto::TransportIO ------------------------------------------
  bool gatt_write(proto::Channel ch, const uint8_t *data, size_t len, bool with_response) override {
    NimBLERemoteCharacteristic *c = ch == proto::Channel::CMD ? cmd_ : data_w_;
    if (c == nullptr || link_ != Link::SESSION || !client_->isConnected()) return false;
    // NimBLE writes block until done, so completion is known right here. It
    // is reported from loop_() to keep the transport out of re-entrancy.
    bool ok = c->writeValue(data, len, with_response);
    for (int i = 0; !ok && !with_response && i < 3 && client_->isConnected(); i++) {
      vTaskDelay(pdMS_TO_TICKS(20));  // out of buffers: give the stack a moment
      ok = c->writeValue(data, len, false);
    }
    write_ok_ = ok;
    write_done_ = true;
    return true;
  }

  // ---- NimBLEClientCallbacks (NimBLE host task) ------------------------------------
  void onDisconnect(NimBLEClient *) override { link_lost_ = true; }
  uint32_t onPassKeyRequest() override {
    logf(1, "Pairing: the panel asks for a code -- not supported");
    return 0;
  }
  bool onConfirmPIN(uint32_t pin) override {
    logf(2, "Pairing: panel shows %06u -- confirming", (unsigned)pin);
    return true;
  }
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    logf(3, "Encryption: encrypted=%d bonded=%d authenticated=%d key size %u", desc->sec_state.encrypted,
         desc->sec_state.bonded, desc->sec_state.authenticated, (unsigned)desc->sec_state.key_size);
  }

 private:
  enum class Link : uint8_t { NO_BLE, WAIT_ADV, CONNECTING, ENCRYPTING, SUBSCRIBING, SESSION };

  struct Command {
    enum Kind : uint8_t { PAIR, UNPAIR, RECONNECT, WRITE, TARGET, DUMP, RESTORE, FROST } kind;
    char topic[24];
    char param[24];
    int32_t value;
    uint16_t dest;
  };
  struct Rx {
    uint8_t ch;  // 0 = CMD, 1 = DATA_R
    uint16_t len;
    uint8_t data[256];
  };
  struct Adv {
    NimBLEAddress addr;
    uint8_t msb[6];
    uint64_t key;
    std::string name;
    int rssi;
    bool has_uuid;
  };
  // Pairing candidates: an armed panel advertises without a name, an idle one
  // puts its name in the scan response. Tracked per (rotating) address.
  struct Candidate {
    uint32_t first_seen = 0;
    uint32_t last_seen = 0;
    uint8_t count = 0;
    bool named = false;
    uint32_t retry_after = 0;  // after a refusal/failure, wait before trying again
  };
  struct Bond {
    ble_addr_t ble;     // identity address as NimBLE stores it
    uint8_t addr[6];    // same, MSB first
    uint8_t irk[16];
    bool has_irk;
  };
  struct IdentityPref {
    uint32_t magic;
    uint8_t muid[16];
    uint8_t uuid[16];
  };
  static const uint32_t IDENTITY_MAGIC = 0x54524D31;  // "TRM1"

  struct Settings {             // what is restored after a panel reset; -1 = unknown
    uint32_t magic;
    int16_t mode;               // RoomClimate.Mode
    int16_t target;             // AirHeating.TgtTemp (0.1 °C)
    int16_t diesel;             // EnergySrc.DieselLevel
    int16_t electric;           // EnergySrc.ElectricLevel
    int16_t water;              // WaterHeating on (1) / off (0)
    int16_t water_mode;         // WaterHeating.Mode
    int16_t fan;                // AirCirculation.FanLevel
    int16_t heat_mode;          // AirHeating.Mode: 0 fast, 1 comfort
  };
  static const uint32_t SETTINGS_MAGIC = 0x54535432;  // "TST2"

  // Reconnect backoff after failed attempts (a successful session resets it).
  static constexpr uint32_t BACKOFF_MS[5] = {2000, 5000, 15000, 30000, 45000};
  static const uint32_t PAIRING_TIMEOUT_MS = 120000;
  static const uint32_t SUPERVISION_TIMEOUT_MS = 20000;
  static const uint32_t SOFT_RESTART_DELAY_MS = 3000;
  // The heater and the panel send each temperature twice, 0.3 s apart and
  // 0.4 °C apart: publish only once the bus has been quiet this long.
  static const uint32_t SETTLE_MS = 800;

  static void task_(void *arg) {
    auto *self = static_cast<Panel *>(arg);
    for (;;) {
      self->loop_();
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }

  bool post_(Command::Kind kind, const char *topic = "", const char *param = "", int32_t value = 0,
             uint16_t dest = 0) {
    if (cmd_q_ == nullptr) return false;
    Command c{};
    c.kind = kind;
    strncpy(c.topic, topic, sizeof(c.topic) - 1);
    strncpy(c.param, param, sizeof(c.param) - 1);
    c.value = value;
    c.dest = dest;
    return xQueueSend(cmd_q_, &c, 0) == pdTRUE;
  }

  // ---- identity (stable across reboots: the panel lists every new one) --------
  void load_identity_(const char *user_name) {
    Preferences prefs;
    prefs.begin("truma", false);
    IdentityPref id{};
    if (prefs.getBytes("id", &id, sizeof(id)) != sizeof(id) || id.magic != IDENTITY_MAGIC) {
      id.magic = IDENTITY_MAGIC;
      esp_fill_random(id.muid, sizeof(id.muid));
      esp_fill_random(id.uuid, sizeof(id.uuid));
      // RFC 4122 version 4 / variant bits
      id.muid[6] = (id.muid[6] & 0x0F) | 0x40;
      id.muid[8] = (id.muid[8] & 0x3F) | 0x80;
      id.uuid[6] = (id.uuid[6] & 0x0F) | 0x40;
      id.uuid[8] = (id.uuid[8] & 0x3F) | 0x80;
      prefs.putBytes("id", &id, sizeof(id));
    }
    panel_name_ = prefs.getString("panel", "").c_str();
    restore_enabled_ = prefs.getBool("restore", true);
    frost_enabled_ = prefs.getBool("frost", true);
    Settings saved{};
    if (prefs.getBytes("settings", &saved, sizeof(saved)) == sizeof(saved) && saved.magic == SETTINGS_MAGIC)
      remembered_ = saved;
    prefs.end();
    session_.identity.user_name = user_name;
    session_.identity.muid = uuid_str(id.muid, true);
    session_.identity.uuid = uuid_str(id.uuid, false);
    logf(3, "App identity %s", session_.identity.muid.c_str());
  }

  void save_panel_name_(const std::string &name) {
    Preferences prefs;
    prefs.begin("truma", false);
    prefs.putString("panel", name.c_str());
    prefs.end();
    panel_name_ = name;
  }

  // ---- bonds -----------------------------------------------------------------------
  void load_bonds_() {
    bonds_.clear();
    int n = NimBLEDevice::getNumBonds();
    for (int i = 0; i < n; i++) {
      NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
      Bond b{};
      memcpy(b.ble.val, a.getNative(), 6);
      b.ble.type = a.getType();
      to_msb(a, b.addr);
      ble_store_key_sec key{};
      key.peer_addr = b.ble;
      ble_store_value_sec val{};
      if (ble_store_read_peer_sec(&key, &val) == 0 && val.irk_present) {
        b.has_irk = true;
        memcpy(b.irk, val.irk, 16);
      }
      bonds_.push_back(b);
    }
  }

  void remove_bonds_() {
    save_panel_name_("");
    load_bonds_();
    for (auto &b : bonds_) {
      logf(2, "Removing bond %s", addr_str(b.addr).c_str());
      NimBLEDevice::deleteBond(NimBLEAddress(b.ble));
    }
    bonds_.clear();
  }

  bool is_our_panel_(const uint8_t addr[6], const std::string &name) const {
    for (auto &b : bonds_) {
      if (memcmp(b.addr, addr, 6) == 0) return true;
      if (b.has_irk && proto::rpa_matches(addr, b.irk, aes128_ecb)) return true;
    }
    // The panel's name carries part of its identity address ("Truma iNetX-55A77E"),
    // so it is unique enough to recognise the panel if its address ever changes.
    return !bonds_.empty() && !panel_name_.empty() && name == panel_name_;
  }

  // ---- main loop (Truma task) -----------------------------------------------------
  void loop_() {
    uint32_t now = millis();
    handle_commands_();
    drain_rx_(now);

    if (link_lost_) {
      link_lost_ = false;
      if (link_ == Link::SESSION) record_disconnect_();
      link_down_("disconnected");
    }
    flush_writes_();

    now = millis();
    if (pairing_ && (int32_t)(now - pairing_until_) >= 0 && link_ == Link::WAIT_ADV) {
      pairing_ = false;
      if (seen_named_ == 0 && seen_nameless_ == 0)
        logf(1, "Pairing window closed: no Truma panel was heard at all. Is it on, in range, and not "
                "connected to another device?");
      else if (seen_nameless_ == 0)
        logf(1, "Pairing window closed: the panel was heard %u times, but always with its name -- it was "
                "never in add-device mode while the window was open", seen_named_);
      else
        logf(1, "Pairing window closed without a successful pairing (%u nameless / %u named advertisements)",
             seen_nameless_, seen_named_);
    }

    if (link_ == Link::WAIT_ADV) search_();

    now = millis();
    if (soft_restart_at_ != 0 && (int32_t)(now - soft_restart_at_) >= 0) {
      soft_restart_at_ = 0;
      if (link_ == Link::SESSION) {
        transport_.reset();
        session_.start(now);
      }
    }
    if (rx_overflow_) {
      rx_overflow_ = false;
      soft_restart_("notification queue overflow");
    }

    transport_.loop(now);
    session_.loop(now);
    flush_writes_();

    now = millis();
    if (conn_params_at_ != 0 && (int32_t)(now - conn_params_at_) >= 0) {
      conn_params_at_ = 0;
      request_robust_conn_params_();
    }
    if (read_params_at_ != 0 && (int32_t)(now - read_params_at_) >= 0) {
      read_params_at_ = 0;
      read_link_params_();
    }
    if (restore_check_at_ != 0 && (int32_t)(now - restore_check_at_) >= 0) {
      restore_check_at_ = 0;
      if (session_.running()) restore_check_();
    } else if (restore_check_at_ == 0 && session_.running() &&
               (int32_t)(now - last_capture_) > (int32_t)CAPTURE_EVERY_MS) {  // signed: may lie ahead
      last_capture_ = now;
      capture_settings_();
    }
    if (frost_check_at_ != 0 && (int32_t)(now - frost_check_at_) >= 0) {
      frost_check_at_ = (now + FROST_EVERY_MS) | 1;
      if (session_.running()) frost_check_();
    }
    publish_state_(false);
  }

  // ---- remember / restore settings ----------------------------------------------
  // The heater falls back to factory settings (off, 18 °C) when its power is
  // cut. The ESP remembers the last settings it saw and writes them back when
  // the panel returns in that factory state. If someone changed the settings
  // at the panel while the ESP was away, those are kept.
  Settings read_settings_() const {
    Settings st{};
    st.magic = SETTINGS_MAGIC;
    auto get = [this](const char *t, const char *p, uint16_t a) -> int16_t {
      const proto::CborValue *v = num_(t, p, a);
      return v ? (int16_t)v->as_int() : (int16_t)-1;
    };
    st.mode = get("RoomClimate", "Mode", proto::DEV_PANEL);
    st.target = get("AirHeating", "TgtTemp", proto::DEV_HEATER);
    st.diesel = get("EnergySrc", "DieselLevel", proto::DEV_HEATER);
    st.electric = get("EnergySrc", "ElectricLevel", proto::DEV_HEATER);
    const int16_t wa = get("WaterHeating", "Active", proto::DEV_HEATER);
    st.water = wa < 0 ? -1 : (wa != 0 ? 1 : 0);  // 1 heating and 2 idle both mean "on"
    st.water_mode = get("WaterHeating", "Mode", proto::DEV_HEATER);
    st.fan = get("AirCirculation", "FanLevel", proto::DEV_HEATER);
    st.heat_mode = get("AirHeating", "Mode", proto::DEV_HEATER);
    return st;
  }

  static bool same_settings_(const Settings &a, const Settings &b) {
    return a.mode == b.mode && a.target == b.target && a.diesel == b.diesel && a.electric == b.electric &&
           a.water == b.water && a.water_mode == b.water_mode && a.fan == b.fan && a.heat_mode == b.heat_mode;
  }

  void capture_settings_() {
    const proto::CborValue *boost = num_("WaterHeating", "FasterHeatingMode", proto::DEV_HEATER);
    if (boost && boost->as_int() == 1) return;  // boost switches heating off for a while: not a setting
    Settings now_set = read_settings_();
    if (now_set.mode < 0 || now_set.target < 0) return;  // values not complete yet
    if (remembered_.magic == SETTINGS_MAGIC && same_settings_(now_set, remembered_)) return;
    remembered_ = now_set;
    Preferences prefs;
    prefs.begin("truma", false);
    prefs.putBytes("settings", &remembered_, sizeof(remembered_));
    prefs.end();
    logf(2, "Settings remembered: mode %d, target %d, diesel %d, electric %d, water %d/%d, fan %d, heat mode %d",
         remembered_.mode, remembered_.target, remembered_.diesel, remembered_.electric, remembered_.water,
         remembered_.water_mode, remembered_.fan, remembered_.heat_mode);
  }

  void restore_check_() {
    const Settings cur = read_settings_();
    const bool unannounced = unannounced_off_;
    unannounced_off_ = false;
    if (restore_enabled_ && unannounced && remembered_.magic == SETTINGS_MAGIC && cur.mode == 0 && mode_before_loss_ > 0) {
      Settings was = remembered_;
      was.mode = 0;  // the last capture may or may not have seen the mode go off
      if (same_settings_(cur, was)) {
        logf(2, "Heating went off during the link loss without being switched off: restoring mode %d",
             mode_before_loss_);
        session_.write("RoomClimate", "Mode", proto::CborValue::make_int(mode_before_loss_), proto::DEV_PANEL);
        restored_at_ = millis() | 1;
        last_capture_ = millis() + 10000;  // next capture in 20 s: let the panel echo first
        return;
      }
    }
    const bool factory = cur.mode == 0 && cur.target == 180;
    if (!restore_enabled_ || remembered_.magic != SETTINGS_MAGIC || !factory || same_settings_(cur, remembered_)) {
      capture_settings_();  // nothing to restore: the panel's settings are the ones to remember
      return;
    }
    const Settings &r = remembered_;
    logf(2, "Panel came back with factory settings: restoring mode %d, target %.1f, diesel %d, electric %d, water %d/%d",
         r.mode, r.target / 10.0, r.diesel, r.electric, r.water, r.water_mode);
    auto w = [this](const char *t, const char *p, int16_t v, uint16_t dest) {
      if (v >= 0) session_.write(t, p, proto::CborValue::make_int(v), dest);
    };
    // Energy first; the heater keeps at least one source on, so switch the
    // wanted one on before the other one off.
    if (r.electric > 0) {
      w("EnergySrc", "ElectricLevel", r.electric, proto::DEV_HEATER);
      w("EnergySrc", "DieselLevel", r.diesel, proto::DEV_HEATER);
    } else {
      w("EnergySrc", "DieselLevel", r.diesel, proto::DEV_HEATER);
      w("EnergySrc", "ElectricLevel", r.electric, proto::DEV_HEATER);
    }
    w("AirHeating", "TgtTemp", r.target, proto::DEV_HEATER);
    w("AirHeating", "Mode", r.heat_mode, proto::DEV_HEATER);
    w("RoomClimate", "Mode", r.mode, proto::DEV_PANEL);
    if (r.mode == 5) w("AirCirculation", "FanLevel", r.fan, proto::DEV_HEATER);
    if (r.water == 1) w("WaterHeating", "Mode", r.water_mode, proto::DEV_HEATER);
    w("WaterHeating", "Active", r.water, proto::DEV_HEATER);
    restored_at_ = millis() | 1;
    last_capture_ = millis() + 10000;  // next capture in 20 s: let the heater echo first
  }

  // Frost guard: below 5 °C the heating must run, whoever switched it off.
  // Electric 900 W when 230 V is there, else diesel; an electric-only setting
  // without 230 V is switched to diesel too. Never switches back to electric.
  void frost_check_() {
    if (!frost_enabled_) return;
    const proto::CborValue *room = num_("AirHeating", "Temp", proto::DEV_HEATER);
    const int mode = current_mode_();
    if (room == nullptr || mode < 0 || room->as_int() >= FROST_TEMP) return;
    const proto::CborValue *pl = num_("LinePower", "Plugged", proto::DEV_HEATER);
    const proto::CborValue *el = num_("EnergySrc", "ElectricLevel", proto::DEV_HEATER);
    const proto::CborValue *di = num_("EnergySrc", "DieselLevel", proto::DEV_HEATER);
    const bool plugged = pl != nullptr && pl->as_int() == 1;
    const bool electric_only = el != nullptr && el->as_int() > 0 && (di == nullptr || di->as_int() == 0);
    auto w = [this](const char *t, const char *p, int v, uint16_t dest) {
      session_.write(t, p, proto::CborValue::make_int(v), dest);
    };
    // The heater keeps at least one source on: switch the wanted one on first.
    auto use_diesel = [&]() { w("EnergySrc", "DieselLevel", 1, proto::DEV_HEATER); w("EnergySrc", "ElectricLevel", 0, proto::DEV_HEATER); };
    auto use_electric = [&]() { w("EnergySrc", "ElectricLevel", 1, proto::DEV_HEATER); w("EnergySrc", "DieselLevel", 0, proto::DEV_HEATER); };
    if (mode == 0) {
      logf(2, "%s frost guard: room %.1f °C, heating off -> heating to %.0f °C on %s", clock_[0] ? clock_ : "?",
           room->as_double() / 10.0, FROST_TEMP / 10.0, plugged ? "electric 900 W" : "diesel (no 230 V)");
      if (plugged) use_electric(); else use_diesel();
      w("AirHeating", "TgtTemp", FROST_TEMP, proto::DEV_HEATER);
      w("RoomClimate", "Mode", 3, proto::DEV_PANEL);
    } else if ((mode == 3 || mode == 4) && electric_only && !plugged) {
      logf(2, "%s frost guard: room %.1f °C, electric only without 230 V -> diesel", clock_[0] ? clock_ : "?",
           room->as_double() / 10.0);
      use_diesel();
    } else {
      return;
    }
    frost_at_ = millis() | 1;
    last_capture_ = millis() + 10000;  // let the heater echo before remembering
  }

  void flush_writes_() {
    while (write_done_) {
      write_done_ = false;
      transport_.on_write_complete(write_ok_, millis());  // may issue the next write
    }
  }

  void handle_commands_() {
    Command c;
    while (xQueueReceive(cmd_q_, &c, 0) == pdTRUE) {
      switch (c.kind) {
        case Command::PAIR: start_pairing_(); break;
        case Command::UNPAIR: unpair_(); break;
        case Command::RECONNECT: reconnect_(); break;
        case Command::WRITE:
          session_.write(c.topic, c.param, proto::CborValue::make_int(c.value), c.dest);
          break;
        case Command::TARGET:
          session_.write(setpoint_topic_(current_mode_()), "TgtTemp", proto::CborValue::make_int(c.value), 0);
          break;
        case Command::DUMP: dump_bus_(); break;
        case Command::RESTORE: {
          restore_enabled_ = c.value != 0;
          Preferences prefs;
          prefs.begin("truma", false);
          prefs.putBool("restore", restore_enabled_);
          prefs.end();
          logf(2, "Restore after panel reset %s", restore_enabled_ ? "on" : "off");
          break;
        }
        case Command::FROST: {
          frost_enabled_ = c.value != 0;
          Preferences prefs;
          prefs.begin("truma", false);
          prefs.putBool("frost", frost_enabled_);
          prefs.end();
          logf(2, "Frost guard %s", frost_enabled_ ? "on" : "off");
          if (frost_enabled_) frost_check_at_ = (millis() + 1000) | 1;
          break;
        }
      }
    }
  }

  void dump_bus_() {
    // A failed allocation throws std::bad_alloc, which aborts this task and
    // reboots the ESP: only build the dump when memory allows, and catch.
    static const size_t DUMP_RESERVE = 6144;  // ~4.6 KB with a Combi: no piecewise regrowth
    std::string out;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < DUMP_RESERVE + 4096) {
      out = "(not enough free memory for the dump right now)\n";
    } else {
      try {
        out.reserve(DUMP_RESERVE);
        char head[48];
        for (auto &d : session_.bus.devices) {
          for (auto &p : d.second.params) {
            snprintf(head, sizeof(head), "0x%04X %s = ", d.first, p.first.c_str());
            out += head;
            out += p.second.value.to_string();
            out += '\n';
          }
        }
        if (out.empty()) out = "(no values yet)\n";
      } catch (const std::bad_alloc &) {
        std::string().swap(out);
        out = "(out of memory while building the dump)\n";
      }
    }
    if (xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(200)) == pdTRUE) {
      bus_dump_.swap(out);
      xSemaphoreGive(state_mutex_);
    }
    dump_seq_ = dump_seq_ + 1;
  }

  void drain_rx_(uint32_t now) {
    Rx rx;
    while (xQueueReceive(rx_q_, &rx, 0) == pdTRUE) {
      if (rx.ch == 0)
        transport_.on_cmd_notify(rx.data, rx.len, now);
      else
        transport_.on_data_notify(rx.data, rx.len, now);
    }
  }

  // NimBLE host task: copy and hand over to the Truma task.
  void on_notify_(uint8_t ch, const uint8_t *data, size_t len) {
    Rx rx;
    rx.ch = ch;
    rx.len = (uint16_t)(len > sizeof(rx.data) ? sizeof(rx.data) : len);
    memcpy(rx.data, data, rx.len);
    if (xQueueSend(rx_q_, &rx, 0) != pdTRUE) rx_overflow_ = true;
  }

  void set_link_(Link l) {
    link_ = l;
    link_since_ = millis();
    publish_state_(true);  // setup steps block this task: show them right away
  }

  // ---- advertisements --------------------------------------------------------------
  void search_() {
    const uint32_t now = millis();
    if (bonds_.empty() && !pairing_) return;  // nothing to look for
    const bool waiting = (int32_t)(now - next_connect_ok_) < 0;
    // While backing off, scan only if another address of the panel may be
    // worth a try right away (see accept_()).
    if (waiting && !pairing_ && last_failed_addr_ == 0) return;
    if ((int32_t)(now - next_scan_ok_) < 0) return;

    std::vector<Adv> advs;
    xSemaphoreTake(gap_lock_, portMAX_DELAY);
    NimBLEScan *scan = NimBLEDevice::getScan();
    scan->setActiveScan(true);  // the panel's name is in the scan response
    scan->setInterval(pairing_ ? 100 : 160);
    scan->setWindow(pairing_ ? 50 : 40);
    NimBLEScanResults res = scan->start(3, false);  // blocking, seconds
    for (int i = 0; i < res.getCount(); i++) {
      NimBLEAdvertisedDevice d = res.getDevice(i);
      Adv a;
      a.addr = d.getAddress();
      to_msb(a.addr, a.msb);
      a.key = addr_key(a.msb);
      a.name = d.haveName() ? d.getName() : std::string();
      a.rssi = d.getRSSI();
      a.has_uuid = false;
      // Panels advertise UUIDs from Truma's range fc3100xx-f3b2-11e8-8eb2-f2801f1b9fd1
      // (0000, 0001, 0002 and 0006 have been seen): accept the whole range.
      for (int u = 0; u < d.getServiceUUIDCount(); u++) {
        std::string s = d.getServiceUUID(u).toString();
        if (s.size() == 36 && strncasecmp(s.c_str(), "fc31", 4) == 0 &&
            strcasecmp(s.c_str() + 8, "-f3b2-11e8-8eb2-f2801f1b9fd1") == 0) {
          a.has_uuid = true;
          break;
        }
      }
      advs.push_back(a);
    }
    scan->clearResults();
    xSemaphoreGive(gap_lock_);
    next_scan_ok_ = millis() + (pairing_ ? 200 : 1000);

    // Decide like the original does per advertisement; among several
    // acceptable ones take the strongest (nameless first while pairing).
    auto better = [this](const Adv &x, const Adv &y) {
      if (pairing_ && x.name.empty() != y.name.empty()) return x.name.empty();
      return x.rssi > y.rssi;
    };
    const Adv *best = nullptr;
    for (auto &a : advs)
      if (accept_(a) && (best == nullptr || better(a, *best))) best = &a;
    if (best != nullptr) connect_(*best);
  }

  bool accept_(const Adv &a) {
    const uint32_t now = millis();
    const bool has_name = strncasecmp(a.name.c_str(), "Truma iNetX", 11) == 0;
    bool is_identity = false;
    for (auto &b : bonds_) is_identity |= memcmp(b.addr, a.msb, 6) == 0;
    if (!a.has_uuid && !has_name && !is_identity) return false;

    {
      // One line per Truma address every 10 s: what the ESP actually hears.
      auto &last = adv_log_[a.key];
      if (last == 0 || now - last > 10000) {
        last = now;
        logf(3, "Truma advertisement from %s, RSSI %d, name '%s', service UUID %s", addr_str(a.msb).c_str(), a.rssi,
             a.name.c_str(), a.has_uuid ? "yes" : "no");
      }
      if (adv_log_.size() > 32) adv_log_.clear();
    }
    if (pairing_) {
      if (a.name.empty())
        seen_nameless_++;
      else
        seen_named_++;
    }
    const bool waiting = (int32_t)(now - next_connect_ok_) < 0;

    if (pairing_) {
      if (waiting) return false;
      // Only a panel in add-device mode accepts a new bond. Nameless panels
      // are preferred (some models drop the name when armed, which keeps a
      // neighbour's idle panel out of the way); otherwise named ones are tried.
      if (candidates_.size() > 32 && candidates_.find(a.key) == candidates_.end()) {
        for (auto it = candidates_.begin(); it != candidates_.end();)
          it = (now - it->second.last_seen > 30000) ? candidates_.erase(it) : std::next(it);
      }
      Candidate &c = candidates_[a.key];
      if (c.count == 0) c.first_seen = now;
      c.last_seen = now;
      if (c.count < 255) c.count++;
      if (a.name.empty())
        last_nameless_at_ = now;
      else
        c.named = true;
      if (c.retry_after != 0 && (int32_t)(now - c.retry_after) < 0) return false;  // failed recently
      if (!c.named) {
        logf(2, "Pairing: found a Truma panel without name at %s (RSSI %d)", addr_str(a.msb).c_str(), a.rssi);
        return true;
      }
      if (now - pairing_started_ > 5000 && (last_nameless_at_ == 0 || now - last_nameless_at_ > 5000)) {
        // Other panels keep their name in add-device mode. With no nameless
        // panel around, try the named one; one that is not armed refuses.
        logf(2, "Pairing: trying Truma panel '%s' at %s (RSSI %d)", a.name.c_str(), addr_str(a.msb).c_str(), a.rssi);
        return true;
      }
      return false;
    }

    if (bonds_.empty()) return false;
    if (!(is_identity || is_our_panel_(a.msb, a.name))) {
      if (now - last_unpaired_log_ > 60000) {
        last_unpaired_log_ = now;
        logf(3, "Ignoring another Truma panel at %s (not ours)", addr_str(a.msb).c_str());
      }
      return false;
    }
    ours_seen_[a.key] = now;
    if (ours_seen_.size() > 16) {
      for (auto it = ours_seen_.begin(); it != ours_seen_.end();)
        it = (now - it->second > 120000) ? ours_seen_.erase(it) : std::next(it);
    }
    // After a bond/disconnect the panel can keep advertising an address it no
    // longer accepts connections on ("phantom") while it also advertises a
    // live one -- so rotate away from failures.
    auto f = fail_at_.find(a.key);
    const bool failed_recently = f != fail_at_.end() && now - f->second < 90000;
    bool alternative = false;
    for (auto &o : ours_seen_) {
      if (o.first == a.key || now - o.second > 30000) continue;
      auto of = fail_at_.find(o.first);
      if (of == fail_at_.end() || now - of->second >= 90000) alternative = true;
    }
    if (failed_recently && alternative) return false;  // a phantom: the live address will come by
    if (waiting && !(last_failed_addr_ != 0 && a.key != last_failed_addr_ && !failed_recently &&
                     now - link_since_ > 1000))
      return false;  // backing off, unless this is a different address than the one that just failed
    logf(3, "Our panel advertises at %s (RSSI %d%s)", addr_str(a.msb).c_str(), a.rssi,
         failed_recently ? ", failed before" : "");
    return true;
  }

  // ---- link setup (blocking, in this task) -------------------------------------------
  void connect_(const Adv &a) {
    current_addr_ = a.key;
    current_name_ = a.name;
    link_lost_ = false;
    set_link_(Link::CONNECTING);
    logf(2, "Connecting to the panel at %s (RSSI %d)", addr_str(a.msb).c_str(), a.rssi);

    xSemaphoreTake(gap_lock_, portMAX_DELAY);
    bool ok = client_->connect(a.addr, true);
    xSemaphoreGive(gap_lock_);
    if (!ok) {
      logf(1, "Connection to the panel failed (rc=%d)", client_->getLastError());
      link_down_("connect failed");
      return;
    }
    fail_at_.erase(current_addr_);
    last_failed_addr_ = 0;
    transport_.reset();

    // Encrypt before touching any characteristic. With a bond this re-uses
    // the stored key, without one it runs Just Works pairing.
    set_link_(Link::ENCRYPTING);
    if (!client_->secureConnection()) {
      auth_failed_(client_->getLastError());
      return;
    }
    auth_failures_ = 0;
    if (pairing_) {
      pairing_ = false;
      if (!current_name_.empty()) save_panel_name_(current_name_);
      logf(2, "Paired with the Truma panel. The panel may show 'something went wrong' -- that only refers to the "
              "app-level handshake and can be ignored.");
    }
    // NimBLE reports the encryption before it persists new keys: give it a moment.
    vTaskDelay(pdMS_TO_TICKS(500));
    load_bonds_();
    {
      NimBLEConnInfo ci = client_->getConnInfo();
      logf(2, "Encrypted (bonded %s, key size %u)", ci.isBonded() ? "yes" : "no", (unsigned)ci.getSecKeySize());
    }

    set_link_(Link::SUBSCRIBING);
    transport_.set_mtu(client_->getMTU());
    logf(3, "MTU %u", (unsigned)client_->getMTU());
    // Search every service: the service UUID differs between panel models.
    NimBLERemoteCharacteristic *cmd = nullptr, *dw = nullptr, *dr = nullptr;
    std::vector<NimBLERemoteService *> *svcs = client_->getServices(true);
    if (svcs != nullptr) {
      for (auto *svc : *svcs) {
        logf(3, "Panel service %s", svc->getUUID().toString().c_str());
        if (!cmd) cmd = svc->getCharacteristic(NimBLEUUID(proto::CHAR_CMD_UUID));
        if (!dw) dw = svc->getCharacteristic(NimBLEUUID(proto::CHAR_DATA_W_UUID));
        if (!dr) dr = svc->getCharacteristic(NimBLEUUID(proto::CHAR_DATA_R_UUID));
      }
    }
    if (cmd == nullptr || dw == nullptr || dr == nullptr) {
      drop_link_("Truma characteristics not found");
      return;
    }
    cmd_ = cmd;
    data_w_ = dw;
    data_r_ = dr;
    // CCCD writes with response, CMD first, then DATA_R (as the original).
    if (!cmd->subscribe(true, [this](NimBLERemoteCharacteristic *, uint8_t *d, size_t l, bool) { on_notify_(0, d, l); },
                        true)) {
      drop_link_("could not enable notifications (CMD)");
      return;
    }
    if (!dr->subscribe(true, [this](NimBLERemoteCharacteristic *, uint8_t *d, size_t l, bool) { on_notify_(1, d, l); },
                       true)) {
      drop_link_("could not enable notifications (DATA_R)");
      return;
    }
    if (!client_->isConnected()) {
      link_down_("disconnected during setup");
      return;
    }
    set_link_(Link::SESSION);
    logf(3, "Notifications on, starting session");
    session_since_ = millis();
    soft_restarts_ = 0;
    session_.start(millis());
  }

  void auth_failed_(int rc) {
    auth_failures_++;
    if (pairing_) {
      auto it = candidates_.find(current_addr_);
      if (it != candidates_.end()) it->second.retry_after = millis() + 8000;
    }
    logf(1, "Encryption/pairing with the panel failed (rc=%d)%s", rc,
         bonds_.empty() ? " -- is the panel in add-device mode?"
                        : " -- if this repeats, the panel has forgotten this device: pair again");
    drop_link_("authentication failed");
    load_bonds_();  // NimBLE deletes a bond whose key the panel no longer has
  }

  // Only for links that never carried a session (setup failed) or on request.
  void drop_link_(const char *reason) {
    if (link_ == Link::WAIT_ADV || link_ == Link::NO_BLE) return;
    logf(1, "Dropping the link: %s", reason);
    if (client_->isConnected()) client_->disconnect();
    link_down_(reason);
  }

  void link_down_(const char *reason) {
    if (link_ == Link::WAIT_ADV || link_ == Link::NO_BLE) return;
    const bool was_running = session_.running();
    if (link_ == Link::CONNECTING && current_addr_ != 0) {
      // The link never came up: remember the address so the next attempt can
      // rotate to another one the panel advertises.
      fail_at_[current_addr_] = millis();
      last_failed_addr_ = current_addr_;
      if (fail_at_.size() > 16) fail_at_.erase(fail_at_.begin());
    }
    session_.stop();
    transport_.reset();
    transport_.set_mtu(23);
    cmd_ = data_w_ = data_r_ = nullptr;
    write_done_ = false;
    soft_restart_at_ = 0;
    conn_params_at_ = 0;
    read_params_at_ = 0;
    restore_check_at_ = 0;
    xQueueReset(rx_q_);

    uint32_t delay;
    if (pairing_) {
      delay = 1000;  // add-device mode does not last long: retry quickly
    } else if (was_running) {
      logf(1, "Connection to the panel lost (%s), reconnecting", reason);
      failures_ = 0;
      delay = BACKOFF_MS[0];
    } else {
      const size_t n = sizeof(BACKOFF_MS) / sizeof(BACKOFF_MS[0]);
      delay = BACKOFF_MS[failures_ < n ? failures_ : n - 1];
      if (failures_ < 250) failures_++;
      logf(3, "Attempt failed (%s), next try in %us", reason, (unsigned)(delay / 1000));
    }
    next_connect_ok_ = millis() + delay;
    set_link_(Link::WAIT_ADV);
  }

  // Restart the Truma session over the existing BLE link: the panel may not
  // accept a new connection after a disconnect until it is power-cycled.
  void soft_restart_(const char *reason) {
    if (link_ != Link::SESSION) {
      drop_link_(reason);
      return;
    }
    if (soft_restart_at_ != 0) return;
    soft_restarts_++;
    logf(1, "Session problem (%s): restarting the session over the existing link (#%u), not disconnecting", reason,
         (unsigned)soft_restarts_);
    session_.stop();
    soft_restart_at_ = (millis() + SOFT_RESTART_DELAY_MS) | 1;  // deferred: may be inside a callback
  }

  // ---- user actions ------------------------------------------------------------------
  void start_pairing_() {
    logf(2, "Pairing mode for %us: put the panel into add-device mode now", (unsigned)(PAIRING_TIMEOUT_MS / 1000));
    if (link_ != Link::WAIT_ADV && link_ != Link::NO_BLE) drop_link_("re-pairing requested");
    remove_bonds_();
    pairing_ = true;
    candidates_.clear();
    fail_at_.clear();
    ours_seen_.clear();
    last_failed_addr_ = 0;
    pairing_started_ = millis();
    last_nameless_at_ = 0;
    seen_named_ = 0;
    seen_nameless_ = 0;
    adv_log_.clear();
    pairing_until_ = millis() + PAIRING_TIMEOUT_MS;
    next_connect_ok_ = millis();
    next_scan_ok_ = millis();
    failures_ = 0;
  }

  void unpair_() {
    pairing_ = false;
    if (link_ != Link::WAIT_ADV && link_ != Link::NO_BLE) drop_link_("unpair requested");
    remove_bonds_();
    logf(2, "Bond removed. Also remove this device from the panel's Bluetooth device list.");
  }

  void reconnect_() {
    next_connect_ok_ = millis();
    next_scan_ok_ = millis();
    failures_ = 0;
    if (link_ == Link::SESSION)
      soft_restart_("restart requested");  // keep the BLE link: the panel would not let us back in
    else if (link_ != Link::WAIT_ADV && link_ != Link::NO_BLE)
      drop_link_("reconnect requested");
  }

  // ---- link parameters -----------------------------------------------------------------
  void request_robust_conn_params_() {
    if (link_ != Link::SESSION) return;
    // Keep the 8.75-11.25 ms interval (the panel rejects longer ones), raise
    // only the supervision timeout.
    logf(2, "Requesting supervision timeout %u ms", (unsigned)SUPERVISION_TIMEOUT_MS);
    client_->updateConnParams(7, 9, 0, SUPERVISION_TIMEOUT_MS / 10);
    read_params_at_ = (millis() + 5000) | 1;
  }

  void read_link_params_() {
    if (link_ != Link::SESSION || !client_->isConnected()) return;
    NimBLEConnInfo ci = client_->getConnInfo();
    snprintf(link_params_, sizeof(link_params_), "interval %.2f ms, latency %u, timeout %.1f s",
             ci.getConnInterval() * 1.25f, (unsigned)ci.getConnLatency(), ci.getConnTimeout() / 100.0f);
    logf(2, "Link parameters in use: %s", link_params_);
    if ((uint32_t)ci.getConnTimeout() * 10u + 500u < SUPERVISION_TIMEOUT_MS)
      logf(1, "The panel did not accept the longer supervision timeout");
  }

  void record_disconnect_() {
    // Heating that goes off while the link is down was not switched off here,
    // unless the panel was shutting down or the mode went off just before.
    const bool off_just_before = logged_mode_ == 0 && last_off_at_ != 0 && millis() - last_off_at_ < 30000;
    mode_before_loss_ = logged_mode_ > 0 ? logged_mode_ : (off_just_before ? last_on_mode_ : 0);
    unannounced_off_ = mode_before_loss_ > 0 && (logged_pwr_ == 1 || logged_pwr_ == 3);
    const uint32_t mins = (millis() - session_since_) / 60000;
    const int r = disc_reason_;
    disc_reason_ = 0;
    char why[40];
    if (r > BLE_HS_ERR_HCI_BASE && r < BLE_HS_ERR_HCI_BASE + 0x100) {
      const int hci = r - BLE_HS_ERR_HCI_BASE;
      const char *text = hci == 0x08 ? " supervision timeout" : hci == 0x13 ? " closed by the panel"
                       : hci == 0x16 ? " closed by the ESP" : hci == 0x22 ? " LL response timeout"
                       : hci == 0x3D ? " MIC failure" : hci == 0x3E ? " not established" : "";
      snprintf(why, sizeof(why), "HCI 0x%02X%s", hci, text);
    } else {
      snprintf(why, sizeof(why), "rc %d", r);
    }
    snprintf(last_disconnect_, sizeof(last_disconnect_),
             "uptime %lus, panel clock %s: link lost after %u h %02u min (%s), %u session restarts",
             (unsigned long)(millis() / 1000), clock_[0] ? clock_ : "?", (unsigned)(mins / 60), (unsigned)(mins % 60),
             why, (unsigned)soft_restarts_);
    logf(1, "%s", last_disconnect_);
  }

  // Log changes of the values that matter for "why is the heating off?",
  // with the panel clock (the ESP has no clock of its own). The last values
  // survive a link loss, so a change across a reconnect is logged too.
  void log_changes_(const State &s) {
    if (const proto::CborValue *t = session_.bus.get("TimeAndDate", "Time", proto::DEV_PANEL))
      if (t->type == proto::CborValue::TEXT) snprintf(clock_, sizeof(clock_), "%s", t->s.c_str());
    const char *at = clock_[0] ? clock_ : "?";
    if (s.mode >= 0 && s.mode != logged_mode_) {
      if (logged_mode_ >= 0) logf(2, "%s mode %d -> %d", at, logged_mode_, s.mode);
      if (logged_mode_ > 0 && s.mode == 0) { last_on_mode_ = logged_mode_; last_off_at_ = millis() | 1; }
      logged_mode_ = s.mode;
    }
    if (s.pwr_mode >= 0 && s.pwr_mode != logged_pwr_) {
      if (logged_pwr_ >= 0) logf(2, "%s panel PwrMode %d -> %d", at, logged_pwr_, s.pwr_mode);
      logged_pwr_ = s.pwr_mode;
    }
    if (s.error[0] && strcmp(s.error, logged_error_) != 0) {
      if (logged_error_[0]) logf(2, "%s heater errors: %.90s", at, s.error);
      snprintf(logged_error_, sizeof(logged_error_), "%s", s.error);
    }
    if (s.panel_error[0] && strcmp(s.panel_error, logged_panel_error_) != 0) {
      if (logged_panel_error_[0]) logf(2, "%s panel errors: %.90s", at, s.panel_error);
      snprintf(logged_panel_error_, sizeof(logged_panel_error_), "%s", s.panel_error);
    }
  }

  // ---- values ------------------------------------------------------------------------------
  const proto::CborValue *num_(const char *topic, const char *param, uint16_t addr = 0) const {
    const proto::CborValue *v = session_.bus.get(topic, param, addr);
    if ((v == nullptr || !v->is_number()) && addr != 0) v = session_.bus.get(topic, param, 0);
    return (v != nullptr && v->is_number()) ? v : nullptr;
  }

  int current_mode_() const {
    const proto::CborValue *m = num_("RoomClimate", "Mode", proto::DEV_PANEL);
    return m ? (int)m->as_int() : -1;
  }

  // Which topic holds the set point for a RoomClimate.Mode value.
  static const char *setpoint_topic_(int mode) {
    switch (mode) {
      case 2:  // cool
      case 6:  // dry
        return "AirCooling";
      case 1:  // auto
        return "RoomClimate";
      default:
        return "AirHeating";
    }
  }

  static void copy_text_(char *dst, size_t n, const proto::CborValue *v) {
    if (v == nullptr) return;
    if (v->type == proto::CborValue::TEXT)
      strncpy(dst, v->s.c_str(), n - 1);
    else if (v->is_number())
      snprintf(dst, n, "%lld", (long long)v->as_int());
  }

  void fill_devices_(State &s, uint32_t now) const {
    for (auto &d : session_.bus.devices) {
      if (s.device_count >= MAX_DEVICES) break;
      if (d.first == proto::DEV_BROADCAST || d.first == proto::DEV_MSG_BROKER) continue;
      if (d.first == session_.assigned_addr()) continue;  // that is us
      DeviceInfo &out = s.devices[s.device_count++];
      out.addr = d.first;
      out.age_s = d.second.last_seen ? (now - d.second.last_seen) / 1000 : 0;
      copy_text_(out.name, sizeof(out.name), session_.bus.get("Identify", "Name", d.first));
      copy_text_(out.serial, sizeof(out.serial), session_.bus.get("Identify", "SerialNr", d.first));
      const proto::CborValue *maj = session_.bus.get("Identify", "SwMaj", d.first);
      const proto::CborValue *min = session_.bus.get("Identify", "SwMin", d.first);
      const proto::CborValue *fx = session_.bus.get("Identify", "SwBgFx", d.first);
      if (maj && min && maj->is_number() && min->is_number())
        snprintf(out.firmware, sizeof(out.firmware), "%d.%d.%d", (int)maj->as_int(), (int)min->as_int(),
                 fx && fx->is_number() ? (int)fx->as_int() : 0);
    }
  }

  void fill_timers_(State &s) const {
    char param[16];
    for (int i = 0; i < MAX_TIMERS; i++) {
      TimerInfo &t = s.timers[i];
      snprintf(param, sizeof(param), "Timer%dState", i + 1);
      if (auto *v = num_("TimerConfig", param, proto::DEV_PANEL)) t.enabled = (int)v->as_int();
      snprintf(param, sizeof(param), "Timer%d", i + 1);
      const proto::CborValue *cfg = session_.bus.get("TimerConfig", param, proto::DEV_PANEL);
      if (cfg == nullptr || cfg->type != proto::CborValue::MAP) continue;
      const proto::CborValue *id = cfg->get("id");
      t.defined = id != nullptr && id->is_number() && id->as_int() != 0;
      copy_text_(t.name, sizeof(t.name), cfg->get("name"));
      copy_text_(t.start, sizeof(t.start), cfg->get("start"));
      copy_text_(t.end, sizeof(t.end), cfg->get("end"));
      if (auto *sym = cfg->get("symbol")) t.symbol = sym->is_number() ? (int)sym->as_int() : -1;
      if (auto *wd = cfg->get("wd")) {
        for (size_t k = 0; k < wd->items.size() && k < 7; k++)
          if (wd->items[k].is_number() && wd->items[k].as_int() != 0) t.weekdays |= (uint8_t)(1 << k);
      }
    }
  }

  void publish_state_(bool force) {
    const uint32_t now = millis();
    // Only every 500 ms, and only once a burst of updates has settled.
    if (!force) {
      if (now - last_publish_ < 500) return;
      if (values_dirty_ && now - last_update_ms_ < SETTLE_MS && now - last_publish_ < 5000) return;
    }
    last_publish_ = now;
    values_dirty_ = false;

    State s;
    s.paired = !bonds_.empty();
    s.pairing = pairing_;
    s.connected = link_ == Link::SESSION && session_.running();
    s.session = session_.state_name();
    if (s.connected) last_live_ = now | 1;
    s.values_age_s = s.connected ? 0 : (last_live_ ? (int32_t)((now - last_live_) / 1000) : -1);
    s.restore_enabled = restore_enabled_;
    s.restored_age_s = restored_at_ ? (int32_t)((now - restored_at_) / 1000) : -1;
    s.frost_enabled = frost_enabled_;
    s.frost_age_s = frost_at_ ? (int32_t)((now - frost_at_) / 1000) : -1;
    if (pairing_ && (int32_t)(pairing_until_ - now) > 0) s.pairing_left_s = (pairing_until_ - now) / 1000;
    strncpy(s.panel_name, panel_name_.c_str(), sizeof(s.panel_name) - 1);
    strncpy(s.last_disconnect, last_disconnect_, sizeof(s.last_disconnect) - 1);
    strncpy(s.link_params, link_params_, sizeof(s.link_params) - 1);

    switch (link_) {
      case Link::NO_BLE: s.status = "starting"; break;
      case Link::WAIT_ADV:
        if (pairing_) s.status = "pairing";
        else if (bonds_.empty()) s.status = "not_paired";
        else if (auth_failures_ >= 3) s.status = "rejected";
        else if ((int32_t)(now - next_connect_ok_) < 0) s.status = "waiting";
        else s.status = "searching";
        break;
      case Link::CONNECTING: s.status = pairing_ ? "pairing_connecting" : "connecting"; break;
      case Link::ENCRYPTING: s.status = pairing_ ? "pairing_bonding" : "encrypting"; break;
      case Link::SUBSCRIBING: s.status = "subscribing"; break;
      case Link::SESSION: s.status = s.connected ? "connected" : "session_starting"; break;
    }

    // Values: mode from the panel, set point and readings from the heater
    // (several devices report AirHeating.Temp; without the address it flips).
    const int mode = current_mode_();
    s.mode = mode;
    const char *sp = setpoint_topic_(mode);
    const proto::CborValue *tgt = num_(sp, "TgtTemp", strcmp(sp, "RoomClimate") == 0 ? 0 : proto::DEV_HEATER);
    if (tgt == nullptr && mode != 3 && mode != 4) tgt = num_("AirHeating", "TgtTemp", proto::DEV_HEATER);
    if (tgt) s.target_temp = tgt->as_double() / 10.0;
    const proto::CborValue *cur = nullptr;
    if (mode == 2 || mode == 6) cur = num_("AirCooling", "Temp");
    if (cur == nullptr) cur = num_("AirHeating", "Temp", proto::DEV_HEATER);
    if (cur) s.room_temp = cur->as_double() / 10.0;
    if (auto *v = num_("AirHeating", "Active", proto::DEV_HEATER)) s.heating_active = (int)v->as_int();
    if (auto *v = num_("AirHeating", "Mode", proto::DEV_HEATER)) s.heating_mode = (int)v->as_int();
    if (auto *v = num_("WaterHeating", "Active", proto::DEV_HEATER)) s.water_active = (int)v->as_int();
    if (auto *v = num_("WaterHeating", "Mode", proto::DEV_HEATER)) s.water_mode = (int)v->as_int();
    if (auto *v = num_("WaterHeating", "Temp", proto::DEV_HEATER)) s.water_temp = v->as_double() / 10.0;
    if (auto *v = num_("EnergySrc", "DieselLevel", proto::DEV_HEATER)) s.diesel_level = (int)v->as_int();
    if (auto *v = num_("EnergySrc", "ElectricLevel", proto::DEV_HEATER)) s.electric_level = (int)v->as_int();
    if (auto *v = num_("AirCirculation", "FanLevel", proto::DEV_HEATER)) s.fan_level = (int)v->as_int();
    if (auto *v = num_("WaterHeating", "FasterHeatingMode", proto::DEV_HEATER)) s.water_boost = (int)v->as_int();
    if (auto *v = num_("WaterHeating", "FasterHeatingModeTime", proto::DEV_HEATER)) s.water_boost_time = (int)v->as_int();
    if (const proto::CborValue *e = session_.bus.get("ErrorReset", "ErrCode", proto::DEV_HEATER))
      strncpy(s.error, e->to_string().c_str(), sizeof(s.error) - 1);
    if (const proto::CborValue *e = session_.bus.get("ErrorReset", "ErrCode", proto::DEV_PANEL))
      strncpy(s.panel_error, e->to_string().c_str(), sizeof(s.panel_error) - 1);
    if (auto *v = num_("LinePower", "Plugged", proto::DEV_HEATER)) s.line_power = (int)v->as_int();
    if (auto *v = num_("PowerMgmt", "PwrMode", proto::DEV_PANEL)) s.pwr_mode = (int)v->as_int();
    if (auto *v = num_("Panel", "Intst", proto::DEV_PANEL)) s.brightness = (int)v->as_int();
    if (auto *v = num_("Panel", "DarkIntst", proto::DEV_PANEL)) s.dark_brightness = (int)v->as_int();
    if (auto *v = num_("Panel", "DisplayTimeout", proto::DEV_PANEL)) s.display_timeout = (int)v->as_int();
    if (auto *v = num_("Panel", "Screensaver", proto::DEV_PANEL)) s.screensaver = (int)v->as_int();
    if (auto *v = num_("Panel", "UserInactiveSince", proto::DEV_PANEL)) s.panel_idle_s = (int32_t)v->as_int();
    fill_devices_(s, now);
    fill_timers_(s);
    if (s.connected) log_changes_(s);

    if (xSemaphoreTake(state_mutex_, pdMS_TO_TICKS(50)) == pdTRUE) {
      snapshot_ = s;
      xSemaphoreGive(state_mutex_);
    }
  }

  // ---- members ---------------------------------------------------------------------------
  proto::Transport transport_;
  proto::Session session_;
  NimBLEClient *client_ = nullptr;
  NimBLERemoteCharacteristic *cmd_ = nullptr;
  NimBLERemoteCharacteristic *data_w_ = nullptr;
  NimBLERemoteCharacteristic *data_r_ = nullptr;

  SemaphoreHandle_t gap_lock_ = nullptr;
  SemaphoreHandle_t state_mutex_ = nullptr;
  QueueHandle_t cmd_q_ = nullptr;
  QueueHandle_t rx_q_ = nullptr;
  State snapshot_;

  static const uint32_t RESTORE_SETTLE_MS = 5000;
  static const uint32_t CAPTURE_EVERY_MS = 10000;
  Settings remembered_{};
  bool restore_enabled_ = true;
  uint32_t restore_check_at_ = 0;
  uint32_t last_capture_ = 0;
  uint32_t restored_at_ = 0;
  static const int FROST_TEMP = 50;                     // 0.1 °C
  static const uint32_t FROST_FIRST_MS = 60000;         // first check after connecting
  static const uint32_t FROST_EVERY_MS = 15 * 60000;
  bool frost_enabled_ = true;
  uint32_t frost_check_at_ = 0;
  uint32_t frost_at_ = 0;
  bool unannounced_off_ = false;    // the last link loss came without a switch-off
  int mode_before_loss_ = 0;
  int last_on_mode_ = 0;            // mode before the last live switch-off
  uint32_t last_off_at_ = 0;
  uint32_t last_live_ = 0;  // millis() when the session last ran
  std::string bus_dump_;             // guarded by state_mutex_
  volatile uint32_t dump_seq_ = 0;

  volatile bool link_lost_ = false;    // set by the NimBLE host task
  volatile bool rx_overflow_ = false;  // set by the NimBLE host task
  bool write_done_ = false;
  bool write_ok_ = false;

  Link link_ = Link::NO_BLE;
  uint32_t link_since_ = 0;
  uint32_t next_connect_ok_ = 0;
  uint32_t next_scan_ok_ = 0;
  uint8_t failures_ = 0;
  uint8_t auth_failures_ = 0;

  std::vector<Bond> bonds_;
  std::string panel_name_;
  std::string current_name_;
  uint64_t current_addr_ = 0;

  bool pairing_ = false;
  uint32_t pairing_started_ = 0;
  uint32_t pairing_until_ = 0;
  uint32_t last_nameless_at_ = 0;
  uint16_t seen_named_ = 0;
  uint16_t seen_nameless_ = 0;
  uint32_t last_unpaired_log_ = 0;
  std::map<uint64_t, Candidate> candidates_;
  std::map<uint64_t, uint32_t> adv_log_;
  std::map<uint64_t, uint32_t> ours_seen_;
  std::map<uint64_t, uint32_t> fail_at_;
  uint64_t last_failed_addr_ = 0;

  uint32_t soft_restart_at_ = 0;
  uint32_t soft_restarts_ = 0;
  uint32_t session_since_ = 0;
  uint32_t conn_params_at_ = 0;
  uint32_t read_params_at_ = 0;
  char last_disconnect_[128] = "";
  char link_params_[64] = "";
  ble_gap_event_listener gap_listener_{};
  volatile int disc_reason_ = 0;    // NimBLE reason of the last panel disconnect (0x200 + HCI code)
  char clock_[6] = "";              // panel clock "HH:MM" as last reported
  int logged_mode_ = -1;            // last values written to the log
  int logged_pwr_ = -1;
  char logged_error_[128] = "";
  char logged_panel_error_[128] = "";

  uint32_t last_update_ms_ = 0;
  uint32_t last_publish_ = 0;
  bool values_dirty_ = false;
};

}  // namespace truma_ble
