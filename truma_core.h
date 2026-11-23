// Truma iNet X protocol core -- platform independent.
//
// Everything in this file and truma_core.cpp is plain C++17 with no ESP-IDF
// or ESPHome dependency, so it can be compiled and tested on a PC
// (see tests/). The ESPHome glue lives in truma_inetx.{h,cpp}.
//
// The wire format was reimplemented from the protocol description and the
// measurements documented in rpodgorny/hass-truma-inetx (GPL-3.0) which in
// turn builds on the reverse engineering of daaaaan/truma-inetx-ble.
#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace truma_inetx_proto {

// ---------------------------------------------------------------------------
// Logging hook. The ESPHome glue points this at ESP_LOGx, tests at printf.
// level: 0=error 1=warn 2=info 3=debug 4=verbose
using LogFn = void (*)(int level, const char *msg);
void set_log_fn(LogFn fn);
void logf(int level, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// ---------------------------------------------------------------------------
// Protocol constants
static const char *const SERVICE_UUID = "fc310002-f3b2-11e8-8eb2-f2801f1b9fd1";
static const char *const CHAR_CMD_UUID = "fc314001-f3b2-11e8-8eb2-f2801f1b9fd1";
static const char *const CHAR_DATA_W_UUID = "fc314002-f3b2-11e8-8eb2-f2801f1b9fd1";
static const char *const CHAR_DATA_R_UUID = "fc314003-f3b2-11e8-8eb2-f2801f1b9fd1";

static const uint16_t DEV_BROADCAST = 0xFFFF;
static const uint16_t DEV_MSG_BROKER = 0x0000;
static const uint16_t DEV_PANEL = 0x0101;
static const uint16_t DEV_HEATER = 0x0201;
static const uint16_t DEV_APP_DEFAULT = 0x0500;

static const uint8_t CTRL_REGISTRATION = 0x01;
static const uint8_t CTRL_MBP = 0x03;

static const uint8_t MBP_INFO = 0x00;
static const uint8_t MBP_WRITE = 0x01;
static const uint8_t MBP_SUBSCRIBE = 0x02;
static const uint8_t MBP_PARAM_DISC = 0x04;
static const uint8_t MBP_PARAM_DISC_RESP = 0x84;

static const uint8_t TRANSPORT_INIT = 0x01;
static const uint8_t TRANSPORT_CONFIRM = 0x03;
static const uint8_t TRANSPORT_READY = 0x81;
static const uint8_t TRANSPORT_MSG_ANNOUNCE = 0x83;
static const uint8_t TRANSPORT_ACK = 0xF0;

// ---------------------------------------------------------------------------
// Minimal CBOR value + codec (RFC 8949 subset: everything the panel sends).
struct CborValue {
  enum Type : uint8_t { NONE, INT, BYTES, TEXT, ARRAY, MAP, BOOL, NUL, FLOAT };
  Type type{NONE};
  int64_t i{0};    // INT, BOOL
  double f{0};     // FLOAT
  std::string s;   // TEXT, BYTES
  std::vector<CborValue> items;  // ARRAY; MAP as key,value,key,value,...

  static CborValue make_int(int64_t v) { CborValue c; c.type = INT; c.i = v; return c; }
  static CborValue make_text(const std::string &v) { CborValue c; c.type = TEXT; c.s = v; return c; }

  bool is_number() const { return type == INT || type == FLOAT || type == BOOL; }
  double as_double() const { return type == FLOAT ? f : static_cast<double>(i); }
  int64_t as_int() const { return type == FLOAT ? static_cast<int64_t>(f) : i; }
  // Map lookup by text key, nullptr if absent / not a map.
  const CborValue *get(const char *key) const;
  // Compact JSON-ish rendering for logs and text sensors.
  std::string to_string() const;
  bool operator==(const CborValue &o) const;
  bool operator!=(const CborValue &o) const { return !(*this == o); }
};

bool cbor_decode(const uint8_t *data, size_t len, CborValue &out, size_t *consumed = nullptr);

class CborWriter {
 public:
  void uint(uint64_t v) { head_(0, v); }
  void integer(int64_t v);
  void text(const std::string &s);
  void array(size_t n) { head_(4, n); }
  void map(size_t n) { head_(5, n); }
  void value(const CborValue &v);
  std::vector<uint8_t> &bytes() { return buf_; }

 private:
  void head_(uint8_t major, uint64_t v);
  std::vector<uint8_t> buf_;
};

// ---------------------------------------------------------------------------
// TruMessageV3 frames
struct Frame {
  uint16_t dest{0};
  uint16_t src{0};
  uint16_t pkt_size{0};
  uint8_t control{0};
  bool has_sub{false};
  uint8_t sub_type{0};
  uint8_t corr_id{0};
  bool has_cbor{false};
  CborValue cbor;  // only maps are kept
};

std::vector<uint8_t> build_frame(uint16_t dest, uint16_t src, uint8_t ctrl, uint8_t sub_type, uint8_t corr_id,
                                 const std::vector<uint8_t> &cbor_payload);
bool parse_frame(const uint8_t *data, size_t len, Frame &out);

std::vector<uint8_t> build_register_frame(uint16_t src);
std::vector<uint8_t> build_subscribe_frame(uint16_t src, const std::vector<std::string> &topics);
std::vector<uint8_t> build_write_frame(uint16_t src, uint16_t dest, const std::string &topic, const std::string &param,
                                       const CborValue &value);
std::vector<uint8_t> build_last_message_frame(uint16_t src);
std::vector<uint8_t> build_param_disc_frame(uint16_t src, uint16_t dest);

extern const std::vector<std::vector<std::string>> TOPIC_BATCHES;
std::set<uint16_t> device_seed();

// ---------------------------------------------------------------------------
// Resolvable private address check (Core spec Vol 3 Part H 2.2.2, ah()).
// aes128: encrypt one 16-byte block, key and data MSB-first.
using Aes128Fn = void (*)(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
// addr: 6 bytes MSB-first (display order). irk: 16 bytes in Bluedroid order
// (LSB-first); both byte orders are tried, so the stack's convention does not
// matter.
bool rpa_matches(const uint8_t addr[6], const uint8_t irk[16], Aes128Fn aes);

// ---------------------------------------------------------------------------
// Transport: the panel's InitDataTransfer / Ready / DataAck handshake on top
// of three GATT characteristics.
enum class Channel : uint8_t { CMD, DATA_W };

class TransportIO {
 public:
  virtual ~TransportIO() = default;
  // Issue one GATT write. Return false if it could not even be queued.
  // Completion (for both write types) is reported via
  // Transport::on_write_complete().
  virtual bool gatt_write(Channel ch, const uint8_t *data, size_t len, bool with_response) = 0;
};

struct TxResult {
  uint16_t dest;
  uint8_t tag;
  bool success;
};

class Transport {
 public:
  explicit Transport(TransportIO *io) : io_(io) {}

  // Frame-level API --------------------------------------------------------
  // probe: silence is an expected answer, do not end the session over it.
  // gap_ms: pause after this frame before the next one is started.
  // tag: opaque, handed back in on_sent so the caller can count acks.
  void queue_frame(std::vector<uint8_t> frame, bool probe, uint32_t gap_ms, uint16_t dest, uint8_t tag = 0);
  bool idle() const { return tx_state_ == TX_IDLE && tx_queue_.empty(); }
  size_t queued() const { return tx_queue_.size() + (tx_state_ == TX_IDLE ? 0 : 1); }

  std::function<void(const Frame &)> on_frame;
  std::function<void(const TxResult &)> on_sent;
  // The stream is no longer trustworthy; the owner must drop the link.
  std::function<void(const char *reason)> on_fatal;

  // Link-level inputs ------------------------------------------------------
  void reset();  // new link or link gone
  void set_mtu(uint16_t mtu) { mtu_ = mtu < 23 ? 23 : mtu; }
  void on_cmd_notify(const uint8_t *data, size_t len, uint32_t now);
  void on_data_notify(const uint8_t *data, size_t len, uint32_t now);
  void on_write_complete(bool ok, uint32_t now);
  void loop(uint32_t now);
  bool invalidated() const { return invalidated_; }

  // Timings (ms), public for tests
  uint32_t ready_timeout{3000};
  uint32_t ack_timeout{3000};
  uint32_t write_timeout{5000};
  uint32_t soft_write_timeout{500};
  uint32_t settle{200};

 private:
  enum TxState : uint8_t { TX_IDLE, TX_WAIT_READY, TX_WAIT_ACK, TX_SETTLE, TX_GAP };
  enum OpTag : uint8_t { OP_NONE, OP_INIT, OP_DATA_LAST };
  struct TxItem {
    std::vector<uint8_t> frame;
    bool probe;
    uint32_t gap_ms;
    uint16_t dest;
    uint8_t tag;
  };
  struct GattOp {
    Channel ch;
    std::vector<uint8_t> data;
    bool rsp;
    bool critical;
    OpTag tag;
  };

  void push_op_(Channel ch, std::vector<uint8_t> data, bool rsp, bool critical, OpTag tag);
  void pump_ops_(uint32_t now);
  void op_done_(bool ok, uint32_t now);
  void finish_(bool success, uint32_t now);
  void fatal_(const char *reason);

  TransportIO *io_;
  uint16_t mtu_{23};
  bool invalidated_{false};

  std::deque<TxItem> tx_queue_;
  TxItem cur_{};
  TxState tx_state_{TX_IDLE};
  uint32_t deadline_{0};
  bool deadline_armed_{false};
  uint8_t expected_{0};
  bool got_expected_{false};
  std::vector<uint8_t> got_bytes_;
  bool success_{false};

  std::deque<GattOp> ops_;
  bool op_busy_{false};
  GattOp op_cur_{};
  uint32_t op_deadline_{0};

  bool rx_announced_{false};
  size_t rx_size_{0};
  std::vector<uint8_t> rx_buf_;
};

// ---------------------------------------------------------------------------
// Bus model: every device behind the panel, and what it last published.
struct BusParam {
  CborValue value;
  uint32_t updated{0};
};
struct BusDevice {
  uint32_t last_seen{0};
  std::map<std::string, BusParam> params;  // "Topic.Param"
};

class Bus {
 public:
  std::map<uint16_t, BusDevice> devices;
  // Returns true if the stored value changed.
  bool update(uint16_t src, const std::string &topic, const std::string &param, const CborValue &v, uint32_t now);
  void note_seen(uint16_t src, uint32_t now) { devices[src].last_seen = now; }
  // addr == 0: the most recently updated publisher.
  const CborValue *get(const std::string &topic, const std::string &param, uint16_t addr = 0) const;
  uint16_t publisher(const std::string &topic, const std::string &param) const;
  std::vector<uint16_t> publishers(const std::string &topic, const std::string &param) const;
  std::set<uint16_t> addresses() const;
};

// ---------------------------------------------------------------------------
// Session: registration, subscription, identity, parameter discovery, and
// the held link afterwards.
struct Identity {
  std::string user_name{"ESPHome"};
  std::string muid;  // upper-case UUID
  std::string uuid;  // lower-case UUID
};

class Session {
 public:
  enum State : uint8_t {
    S_IDLE,
    S_REGISTER,
    S_SUBSCRIBE,
    S_SUBSCRIBE_SETTLE,
    S_IDENTITY,
    S_DISCOVERY,
    S_DISCOVERY_SETTLE,
    S_MEASURE,
    S_RUNNING,
    S_FAILED,
  };

  explicit Session(Transport *t);

  void start(uint32_t now);
  void stop();
  void loop(uint32_t now);
  State state() const { return state_; }
  bool running() const { return state_ == S_RUNNING; }
  const char *state_name() const;
  uint16_t assigned_addr() const { return assigned_; }

  // Queue a parameter write. dest==0 resolves the destination from the bus.
  bool write(const std::string &topic, const std::string &param, const CborValue &value, uint16_t dest = 0);
  uint16_t resolve_dest(const std::string &topic, const std::string &param) const;
  // Ask the tank sensors for a fresh reading.
  void request_measurements();

  Bus bus;
  Identity identity;
  // Seconds since epoch, or 0 if unknown.
  std::function<uint32_t()> unix_time;
  // A value an entity may read has changed.
  std::function<void(uint16_t src, const std::string &topic, const std::string &param, const CborValue &v)> on_update;
  // Session became usable / failed (owner drops the link on failure).
  std::function<void()> on_ready;
  std::function<void(const char *reason)> on_failed;
  bool log_params{false};
  // Log every parameter message in full (all CBOR fields) until this time.
  void log_raw_for(uint32_t ms) { log_raw_until_ = now_ + ms; log_raw_on_ = true; }

  // Timings (ms), public for tests
  uint32_t register_timeout{20000};
  uint32_t subscribe_gap{500};
  uint32_t subscribe_settle{3000};
  uint32_t identity_gap{500};
  uint32_t disc_gap{150};
  uint32_t disc_settle{3000};
  uint32_t stall_timeout{90000};
  uint32_t measure_interval{60000};
  uint32_t startup_timeout{120000};

 private:
  bool log_raw_on_{false};
  uint32_t log_raw_until_{0};
  enum TxTag : uint8_t { TAG_NONE, TAG_DISC, TAG_MEASURE_FRESH, TAG_MEASURE_GREY };
  void handle_frame_(const Frame &f);
  void handle_sent_(const TxResult &r);
  void fail_(const char *reason);
  void enter_(State s, uint32_t now);
  bool discovery_round_();
  void measure_(uint32_t now);

  Transport *t_;
  State state_{S_IDLE};
  uint32_t now_{0};
  uint32_t state_since_{0};
  uint32_t start_time_{0};
  uint32_t wait_until_{0};
  uint32_t last_frame_{0};
  uint32_t next_measure_{0};
  uint16_t assigned_{DEV_APP_DEFAULT};
  bool registered_{false};
  int disc_round_{0};
  std::set<uint16_t> asked_;
  std::set<uint16_t> acked_;
  struct Miss {
    uint8_t count;
    uint32_t seen_at;
  };
  std::map<std::pair<uint16_t, uint8_t>, Miss> misses_;
};

}  // namespace truma_inetx_proto
