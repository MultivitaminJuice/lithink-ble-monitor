#include "truma_core.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>

namespace truma_inetx_proto {

// ---------------------------------------------------------------------------
// Logging
static LogFn g_log = nullptr;
void set_log_fn(LogFn fn) { g_log = fn; }
void logf(int level, const char *fmt, ...) {
  if (g_log == nullptr)
    return;
  char buf[320];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  g_log(level, buf);
}
#define LOGE(...) logf(0, __VA_ARGS__)
#define LOGW(...) logf(1, __VA_ARGS__)
#define LOGI(...) logf(2, __VA_ARGS__)
#define LOGD(...) logf(3, __VA_ARGS__)
#define LOGV(...) logf(4, __VA_ARGS__)

static inline bool time_reached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

// ---------------------------------------------------------------------------
// CBOR
const CborValue *CborValue::get(const char *key) const {
  if (type != MAP)
    return nullptr;
  for (size_t k = 0; k + 1 < items.size(); k += 2) {
    if (items[k].type == TEXT && items[k].s == key)
      return &items[k + 1];
  }
  return nullptr;
}

static void json_escape(std::string &out, const std::string &s) {
  out += '"';
  for (char c : s) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (static_cast<unsigned char>(c) < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", c);
      out += b;
    } else {
      out += c;
    }
  }
  out += '"';
}

static void render(const CborValue &v, std::string &out) {
  char b[32];
  switch (v.type) {
    case CborValue::INT:
      snprintf(b, sizeof(b), "%lld", static_cast<long long>(v.i));
      out += b;
      break;
    case CborValue::FLOAT:
      snprintf(b, sizeof(b), "%g", v.f);
      out += b;
      break;
    case CborValue::BOOL:
      out += v.i ? "true" : "false";
      break;
    case CborValue::TEXT:
      json_escape(out, v.s);
      break;
    case CborValue::BYTES:
      out += "h'";
      for (unsigned char c : v.s) {
        snprintf(b, sizeof(b), "%02x", c);
        out += b;
      }
      out += "'";
      break;
    case CborValue::ARRAY:
      out += '[';
      for (size_t k = 0; k < v.items.size(); k++) {
        if (k)
          out += ',';
        render(v.items[k], out);
      }
      out += ']';
      break;
    case CborValue::MAP:
      out += '{';
      for (size_t k = 0; k + 1 < v.items.size(); k += 2) {
        if (k)
          out += ',';
        render(v.items[k], out);
        out += ':';
        render(v.items[k + 1], out);
      }
      out += '}';
      break;
    case CborValue::NUL:
    case CborValue::NONE:
      out += "null";
      break;
  }
}

std::string CborValue::to_string() const {
  std::string out;
  render(*this, out);
  return out;
}

bool CborValue::operator==(const CborValue &o) const {
  if (type != o.type)
    return false;
  switch (type) {
    case INT:
    case BOOL:
      return i == o.i;
    case FLOAT:
      return f == o.f;
    case TEXT:
    case BYTES:
      return s == o.s;
    case ARRAY:
    case MAP:
      return items == o.items;
    default:
      return true;
  }
}

namespace {
struct Reader {
  const uint8_t *d;
  size_t len;
  size_t pos{0};

  bool byte(uint8_t &b) {
    if (pos >= len)
      return false;
    b = d[pos++];
    return true;
  }
  bool uint_n(size_t n, uint64_t &v) {
    if (len - pos < n)
      return false;
    v = 0;
    for (size_t k = 0; k < n; k++)
      v = (v << 8) | d[pos++];
    return true;
  }
  // Reads an initial byte; for ai==31 sets indef.
  bool head(uint8_t &major, uint8_t &ai, uint64_t &val, bool &indef) {
    uint8_t ib;
    if (!byte(ib))
      return false;
    major = ib >> 5;
    ai = ib & 0x1F;
    indef = false;
    val = 0;
    if (ai < 24) {
      val = ai;
      return true;
    }
    switch (ai) {
      case 24:
        return uint_n(1, val);
      case 25:
        return uint_n(2, val);
      case 26:
        return uint_n(4, val);
      case 27:
        return uint_n(8, val);
      case 31:
        indef = true;
        return true;
      default:
        return false;
    }
  }
  bool peek_break() const { return pos < len && d[pos] == 0xFF; }
};

double half_to_double(uint16_t h) {
  int exp = (h >> 10) & 0x1F;
  int mant = h & 0x3FF;
  double val;
  if (exp == 0)
    val = std::ldexp(mant, -24);
  else if (exp != 31)
    val = std::ldexp(mant + 1024, exp - 25);
  else
    val = mant == 0 ? std::numeric_limits<double>::infinity() : std::numeric_limits<double>::quiet_NaN();
  return (h & 0x8000) ? -val : val;
}

bool decode_item(Reader &r, CborValue &out, int depth) {
  if (depth > 16)
    return false;
  uint8_t major, ai;
  uint64_t val;
  bool indef;
  if (!r.head(major, ai, val, indef))
    return false;
  out = CborValue();
  switch (major) {
    case 0:
      if (indef)
        return false;
      out.type = CborValue::INT;
      out.i = val > static_cast<uint64_t>(INT64_MAX) ? INT64_MAX : static_cast<int64_t>(val);
      return true;
    case 1:
      if (indef)
        return false;
      out.type = CborValue::INT;
      out.i = val > static_cast<uint64_t>(INT64_MAX) ? INT64_MIN : -1 - static_cast<int64_t>(val);
      return true;
    case 2:
    case 3: {
      out.type = major == 2 ? CborValue::BYTES : CborValue::TEXT;
      if (!indef) {
        if (r.len - r.pos < val)
          return false;
        out.s.assign(reinterpret_cast<const char *>(r.d + r.pos), static_cast<size_t>(val));
        r.pos += static_cast<size_t>(val);
        return true;
      }
      while (!r.peek_break()) {
        CborValue chunk;
        if (!decode_item(r, chunk, depth + 1) || chunk.type != out.type)
          return false;
        out.s += chunk.s;
      }
      r.pos++;  // break
      return true;
    }
    case 4:
    case 5: {
      out.type = major == 4 ? CborValue::ARRAY : CborValue::MAP;
      const size_t per = major == 4 ? 1 : 2;
      if (!indef) {
        if (val > r.len)  // cheap sanity bound: every item is >= 1 byte
          return false;
        out.items.resize(static_cast<size_t>(val) * per);
        for (auto &item : out.items)
          if (!decode_item(r, item, depth + 1))
            return false;
        return true;
      }
      while (!r.peek_break()) {
        for (size_t k = 0; k < per; k++) {
          out.items.emplace_back();
          if (!decode_item(r, out.items.back(), depth + 1))
            return false;
        }
      }
      r.pos++;
      return true;
    }
    case 6:  // tag: ignore it, keep the tagged item
      return decode_item(r, out, depth + 1);
    case 7:
      switch (ai) {
        case 20:
        case 21:
          out.type = CborValue::BOOL;
          out.i = ai == 21;
          return true;
        case 25:
          out.type = CborValue::FLOAT;
          out.f = half_to_double(static_cast<uint16_t>(val));
          return true;
        case 26: {
          uint32_t bits = static_cast<uint32_t>(val);
          float fv;
          memcpy(&fv, &bits, 4);
          out.type = CborValue::FLOAT;
          out.f = fv;
          return true;
        }
        case 27: {
          double dv;
          memcpy(&dv, &val, 8);
          out.type = CborValue::FLOAT;
          out.f = dv;
          return true;
        }
        default:
          if (indef)
            return false;  // stray break
          out.type = CborValue::NUL;
          return true;
      }
  }
  return false;
}
}  // namespace

bool cbor_decode(const uint8_t *data, size_t len, CborValue &out, size_t *consumed) {
  Reader r{data, len};
  if (!decode_item(r, out, 0))
    return false;
  if (consumed != nullptr)
    *consumed = r.pos;
  return true;
}

void CborWriter::head_(uint8_t major, uint64_t v) {
  uint8_t m = static_cast<uint8_t>(major << 5);
  if (v < 24) {
    buf_.push_back(m | static_cast<uint8_t>(v));
  } else if (v <= 0xFF) {
    buf_.push_back(m | 24);
    buf_.push_back(static_cast<uint8_t>(v));
  } else if (v <= 0xFFFF) {
    buf_.push_back(m | 25);
    buf_.push_back(static_cast<uint8_t>(v >> 8));
    buf_.push_back(static_cast<uint8_t>(v));
  } else if (v <= 0xFFFFFFFFULL) {
    buf_.push_back(m | 26);
    for (int s = 24; s >= 0; s -= 8)
      buf_.push_back(static_cast<uint8_t>(v >> s));
  } else {
    buf_.push_back(m | 27);
    for (int s = 56; s >= 0; s -= 8)
      buf_.push_back(static_cast<uint8_t>(v >> s));
  }
}

void CborWriter::integer(int64_t v) {
  if (v >= 0)
    head_(0, static_cast<uint64_t>(v));
  else
    head_(1, static_cast<uint64_t>(-1 - v));
}

void CborWriter::text(const std::string &s) {
  head_(3, s.size());
  buf_.insert(buf_.end(), s.begin(), s.end());
}

void CborWriter::value(const CborValue &v) {
  switch (v.type) {
    case CborValue::INT:
      integer(v.i);
      break;
    case CborValue::TEXT:
      text(v.s);
      break;
    case CborValue::BYTES:
      head_(2, v.s.size());
      buf_.insert(buf_.end(), v.s.begin(), v.s.end());
      break;
    case CborValue::BOOL:
      buf_.push_back(v.i ? 0xF5 : 0xF4);
      break;
    case CborValue::FLOAT: {
      uint64_t bits;
      memcpy(&bits, &v.f, 8);
      buf_.push_back(0xFB);
      for (int s = 56; s >= 0; s -= 8)
        buf_.push_back(static_cast<uint8_t>(bits >> s));
      break;
    }
    case CborValue::ARRAY:
      array(v.items.size());
      for (auto &item : v.items)
        value(item);
      break;
    case CborValue::MAP:
      map(v.items.size() / 2);
      for (auto &item : v.items)
        value(item);
      break;
    default:
      buf_.push_back(0xF6);
      break;
  }
}

// ---------------------------------------------------------------------------
// Frames
//
// [0-1] dest LE, [2-3] src LE, [4-5] packet size LE (= payload + 9),
// [6] control type, [7-15] segmentation header (zero), [16] sub type,
// [17] correlation id, [18..] CBOR.
std::vector<uint8_t> build_frame(uint16_t dest, uint16_t src, uint8_t ctrl, uint8_t sub_type, uint8_t corr_id,
                                 const std::vector<uint8_t> &cbor_payload) {
  const size_t payload_len = 2 + cbor_payload.size();
  const uint16_t pkt_size = static_cast<uint16_t>(payload_len + 9);
  std::vector<uint8_t> f;
  f.reserve(16 + payload_len);
  f.push_back(dest & 0xFF);
  f.push_back(dest >> 8);
  f.push_back(src & 0xFF);
  f.push_back(src >> 8);
  f.push_back(pkt_size & 0xFF);
  f.push_back(pkt_size >> 8);
  f.push_back(ctrl);
  f.insert(f.end(), 9, 0);
  f.push_back(sub_type);
  f.push_back(corr_id);
  f.insert(f.end(), cbor_payload.begin(), cbor_payload.end());
  return f;
}

bool parse_frame(const uint8_t *d, size_t len, Frame &out) {
  if (len < 16)
    return false;
  out = Frame();
  out.dest = d[0] | (d[1] << 8);
  out.src = d[2] | (d[3] << 8);
  out.pkt_size = d[4] | (d[5] << 8);
  out.control = d[6];
  if (len > 16) {
    out.has_sub = true;
    out.sub_type = d[16];
    out.corr_id = len > 17 ? d[17] : 0;
  }
  if (len > 18) {
    CborValue v;
    if (cbor_decode(d + 18, len - 18, v) && v.type == CborValue::MAP) {
      out.cbor = std::move(v);
      out.has_cbor = true;
    }
  }
  return true;
}

std::vector<uint8_t> build_register_frame(uint16_t src) {
  CborWriter w;
  w.map(1);
  w.text("pv");
  w.array(2);
  w.uint(5);
  w.uint(1);
  return build_frame(DEV_BROADCAST, src, CTRL_REGISTRATION, 0x01, 0x42, w.bytes());
}

std::vector<uint8_t> build_subscribe_frame(uint16_t src, const std::vector<std::string> &topics) {
  CborWriter w;
  w.map(1);
  w.text("tn");
  w.array(topics.size());
  for (auto &t : topics)
    w.text(t);
  return build_frame(DEV_MSG_BROKER, src, CTRL_MBP, MBP_SUBSCRIBE, 0, w.bytes());
}

std::vector<uint8_t> build_write_frame(uint16_t src, uint16_t dest, const std::string &topic, const std::string &param,
                                       const CborValue &value) {
  CborWriter w;
  w.map(4);
  w.text("tn");
  w.text(topic);
  w.text("pn");
  w.text(param);
  w.text("v");
  w.value(value);
  w.text("id");
  w.uint(0);
  return build_frame(dest, src, CTRL_MBP, MBP_WRITE, 0, w.bytes());
}

std::vector<uint8_t> build_last_message_frame(uint16_t src) {
  CborWriter w;
  w.map(1);
  w.text("LastMessage");
  w.uint(1);
  return build_frame(DEV_PANEL, src, CTRL_MBP, MBP_WRITE, 0, w.bytes());
}

std::vector<uint8_t> build_param_disc_frame(uint16_t src, uint16_t dest) {
  return build_frame(dest, src, CTRL_MBP, MBP_PARAM_DISC, 0, {});
}

const std::vector<std::vector<std::string>> TOPIC_BATCHES = {
    {"AirCirculation", "AirCooling", "AirHeating", "DeviceManagement", "EnergySrc", "ErrorReset", "FreshWater",
     "GasBtl", "GasControl", "GreyWater"},
    {"Identify", "L1Bat", "L2Bat", "LinePower", "MobileIdentity", "PowerSupply", "RoomClimate", "Switches",
     "Temperature", "Transfer"},
    {"VBat", "WaterHeating", "AmbientLight", "Panel", "BatteryMngmt", "Install", "Connect", "TimerConfig",
     "BleDeviceManagement", "BluetoothDevice"},
    {"System", "Resources", "PowerMgmt", "TimeAndDate"},
};

// Only the panel and the heater answer a broadcast discovery; everything else
// (electrical block, tanks, gas bottles, roof AC) must be asked directly.
// Instances are swept because they get renumbered on re-pairing.
std::set<uint16_t> device_seed() {
  std::set<uint16_t> s{DEV_PANEL, DEV_HEATER};
  for (uint16_t i = 1; i <= 8; i++) {
    s.insert(0x0400 | i);
    s.insert(0x0600 | i);
  }
  return s;
}

// ---------------------------------------------------------------------------
// RPA
static bool rpa_try(const uint8_t addr[6], const uint8_t key_msb[16], Aes128Fn aes) {
  uint8_t in[16] = {0};
  uint8_t out[16];
  in[13] = addr[0];
  in[14] = addr[1];
  in[15] = addr[2];
  aes(key_msb, in, out);
  return out[13] == addr[3] && out[14] == addr[4] && out[15] == addr[5];
}

bool rpa_matches(const uint8_t addr[6], const uint8_t irk[16], Aes128Fn aes) {
  if ((addr[0] & 0xC0) != 0x40)
    return false;  // not a resolvable private address
  bool all_zero = true;
  for (int k = 0; k < 16; k++)
    all_zero &= irk[k] == 0;
  if (all_zero)
    return false;
  uint8_t rev[16];
  for (int k = 0; k < 16; k++)
    rev[k] = irk[15 - k];
  return rpa_try(addr, rev, aes) || rpa_try(addr, irk, aes);
}

// ---------------------------------------------------------------------------
// Transport
void Transport::queue_frame(std::vector<uint8_t> frame, bool probe, uint32_t gap_ms, uint16_t dest, uint8_t tag) {
  if (invalidated_) {
    LOGD("transport invalidated, dropping frame to 0x%04X", dest);
    return;
  }
  tx_queue_.push_back(TxItem{std::move(frame), probe, gap_ms, dest, tag});
}

void Transport::reset() {
  invalidated_ = false;
  tx_queue_.clear();
  tx_state_ = TX_IDLE;
  expected_ = 0;
  got_expected_ = false;
  deadline_armed_ = false;
  ops_.clear();
  op_busy_ = false;
  rx_announced_ = false;
  rx_size_ = 0;
  rx_buf_.clear();
}

void Transport::fatal_(const char *reason) {
  if (invalidated_)
    return;
  invalidated_ = true;
  tx_queue_.clear();
  ops_.clear();
  op_busy_ = false;
  tx_state_ = TX_IDLE;
  expected_ = 0;
  LOGW("transport: %s", reason);
  if (on_fatal)
    on_fatal(reason);
}

void Transport::push_op_(Channel ch, std::vector<uint8_t> data, bool rsp, bool critical, OpTag tag) {
  ops_.push_back(GattOp{ch, std::move(data), rsp, critical, tag});
}

void Transport::pump_ops_(uint32_t now) {
  if (invalidated_)
    return;
  if (op_busy_) {
    if (!time_reached(now, op_deadline_))
      return;
    if (op_cur_.rsp) {
      fatal_("a GATT write went unanswered");
      return;
    }
    // Write without response: some stacks never confirm it. Assume it left.
    op_done_(true, now);
    if (invalidated_ || op_busy_)
      return;
  }
  while (!op_busy_ && !ops_.empty() && !invalidated_) {
    op_cur_ = std::move(ops_.front());
    ops_.pop_front();
    op_busy_ = true;
    op_deadline_ = now + (op_cur_.rsp ? write_timeout : soft_write_timeout);
    if (!io_->gatt_write(op_cur_.ch, op_cur_.data.data(), op_cur_.data.size(), op_cur_.rsp)) {
      op_busy_ = false;
      if (op_cur_.critical) {
        LOGD("transport: GATT write could not be issued");
        op_cur_.tag = OP_NONE;
        if (tx_state_ == TX_WAIT_READY || tx_state_ == TX_WAIT_ACK)
          finish_(false, now);
      }
    }
  }
}

void Transport::on_write_complete(bool ok, uint32_t now) {
  if (!op_busy_)
    return;
  op_done_(ok, now);
  pump_ops_(now);
}

void Transport::op_done_(bool ok, uint32_t now) {
  op_busy_ = false;
  if (!ok) {
    if (op_cur_.critical) {
      LOGD("transport: GATT write failed");
      if (tx_state_ == TX_WAIT_READY || tx_state_ == TX_WAIT_ACK)
        finish_(false, now);
    } else {
      LOGD("transport: reply write failed (ignored)");
    }
    return;
  }
  if (op_cur_.tag == OP_INIT && tx_state_ == TX_WAIT_READY) {
    deadline_ = now + ready_timeout;
    deadline_armed_ = true;
  } else if (op_cur_.tag == OP_DATA_LAST && tx_state_ == TX_WAIT_ACK) {
    deadline_ = now + ack_timeout;
    deadline_armed_ = true;
  }
}

void Transport::finish_(bool success, uint32_t now) {
  TxItem done = std::move(cur_);
  expected_ = 0;
  got_expected_ = false;
  deadline_armed_ = false;
  tx_state_ = TX_GAP;
  deadline_ = now + done.gap_ms;
  if (on_sent)
    on_sent(TxResult{done.dest, done.tag, success});
  if (!success && !done.probe) {
    // Ready and DataAck carry no transfer identity: after an unanswered
    // transfer, a late ack could be taken for the next frame's. The only safe
    // move is to end the session (measured upstream, see hass-truma-inetx).
    fatal_("the panel did not acknowledge a frame");
  }
}

void Transport::loop(uint32_t now) {
  pump_ops_(now);
  if (invalidated_)
    return;
  switch (tx_state_) {
    case TX_IDLE: {
      if (tx_queue_.empty())
        return;
      cur_ = std::move(tx_queue_.front());
      tx_queue_.pop_front();
      const size_t n = cur_.frame.size();
      expected_ = TRANSPORT_READY;
      got_expected_ = false;
      deadline_armed_ = false;
      tx_state_ = TX_WAIT_READY;
      push_op_(Channel::CMD, {TRANSPORT_INIT, static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>(n >> 8)}, true,
               true, OP_INIT);
      pump_ops_(now);
      return;
    }
    case TX_WAIT_READY:
      if (got_expected_) {
        got_expected_ = false;
        expected_ = TRANSPORT_ACK;
        deadline_armed_ = false;
        tx_state_ = TX_WAIT_ACK;
        const size_t chunk = mtu_ - 3;
        const auto &f = cur_.frame;
        for (size_t off = 0; off < f.size(); off += chunk) {
          size_t end = off + chunk < f.size() ? off + chunk : f.size();
          push_op_(Channel::DATA_W, std::vector<uint8_t>(f.begin() + off, f.begin() + end), false, true,
                   end == f.size() ? OP_DATA_LAST : OP_NONE);
        }
        pump_ops_(now);
      } else if (deadline_armed_ && time_reached(now, deadline_)) {
        LOGD("transport: timeout waiting for Ready (frame to 0x%04X)", cur_.dest);
        finish_(false, now);
      }
      return;
    case TX_WAIT_ACK:
      if (got_expected_) {
        success_ = got_bytes_.size() >= 2 && got_bytes_[0] == TRANSPORT_ACK && got_bytes_[1] == 0x01;
        tx_state_ = TX_SETTLE;
        deadline_ = now + settle;
      } else if (deadline_armed_ && time_reached(now, deadline_)) {
        LOGD("transport: timeout waiting for DataAck (frame to 0x%04X)", cur_.dest);
        finish_(false, now);
      }
      return;
    case TX_SETTLE:
      if (time_reached(now, deadline_))
        finish_(success_, now);
      return;
    case TX_GAP:
      if (time_reached(now, deadline_))
        tx_state_ = TX_IDLE;
      return;
  }
}

void Transport::on_cmd_notify(const uint8_t *d, size_t len, uint32_t now) {
  if (invalidated_)
    return;
  // 0x83 announces an INCOMING message of the given length -- it is never an
  // acknowledgement of anything we sent.
  if (len >= 3 && d[0] == TRANSPORT_MSG_ANNOUNCE) {
    rx_announced_ = true;
    rx_size_ = d[1] | (d[2] << 8);
    rx_buf_.clear();
    push_op_(Channel::CMD, {TRANSPORT_CONFIRM, 0x00}, true, false, OP_NONE);
    pump_ops_(now);
    return;
  }
  if (len == 0 || len > 4)
    return;
  // Ready/DataAck carry no transfer id; only accept the one opcode the
  // transfer in flight is waiting for.
  if (expected_ != 0 && d[0] == expected_) {
    got_bytes_.assign(d, d + len);
    got_expected_ = true;
  }
}

void Transport::on_data_notify(const uint8_t *d, size_t len, uint32_t now) {
  if (invalidated_)
    return;
  std::vector<uint8_t> msg;
  if (rx_announced_) {
    rx_buf_.insert(rx_buf_.end(), d, d + len);
    if (rx_buf_.size() < rx_size_)
      return;
    if (rx_buf_.size() > rx_size_) {
      LOGW("transport: incoming message overran its announced %u bytes", static_cast<unsigned>(rx_size_));
      rx_announced_ = false;
      rx_buf_.clear();
      return;
    }
    msg.swap(rx_buf_);
    rx_announced_ = false;
  } else {
    msg.assign(d, d + len);
  }
  push_op_(Channel::CMD, {TRANSPORT_ACK, 0x01}, true, false, OP_NONE);
  pump_ops_(now);
  Frame f;
  if (parse_frame(msg.data(), msg.size(), f) && on_frame)
    on_frame(f);
}

// ---------------------------------------------------------------------------
// Bus
bool Bus::update(uint16_t src, const std::string &topic, const std::string &param, const CborValue &v, uint32_t now) {
  BusDevice &dev = devices[src];
  dev.last_seen = now;
  BusParam &p = dev.params[topic + "." + param];
  const bool changed = p.updated == 0 || p.value != v;
  p.value = v;
  p.updated = now == 0 ? 1 : now;
  return changed;
}

const CborValue *Bus::get(const std::string &topic, const std::string &param, uint16_t addr) const {
  const std::string key = topic + "." + param;
  if (addr != 0) {
    auto d = devices.find(addr);
    if (d == devices.end())
      return nullptr;
    auto p = d->second.params.find(key);
    return p == d->second.params.end() ? nullptr : &p->second.value;
  }
  const BusParam *best = nullptr;
  for (auto &d : devices) {
    auto p = d.second.params.find(key);
    if (p != d.second.params.end() && (best == nullptr || static_cast<int32_t>(p->second.updated - best->updated) > 0))
      best = &p->second;
  }
  return best == nullptr ? nullptr : &best->value;
}

uint16_t Bus::publisher(const std::string &topic, const std::string &param) const {
  const std::string key = topic + "." + param;
  uint16_t addr = 0;
  uint32_t when = 0;
  bool found = false;
  for (auto &d : devices) {
    auto p = d.second.params.find(key);
    if (p != d.second.params.end() && (!found || static_cast<int32_t>(p->second.updated - when) > 0)) {
      addr = d.first;
      when = p->second.updated;
      found = true;
    }
  }
  return addr;
}

std::vector<uint16_t> Bus::publishers(const std::string &topic, const std::string &param) const {
  const std::string key = topic + "." + param;
  std::vector<uint16_t> out;
  for (auto &d : devices)
    if (d.second.params.count(key))
      out.push_back(d.first);
  return out;
}

std::set<uint16_t> Bus::addresses() const {
  std::set<uint16_t> out;
  for (auto &d : devices)
    out.insert(d.first);
  return out;
}

// ---------------------------------------------------------------------------
// Session
Session::Session(Transport *t) : t_(t) {
  t_->on_frame = [this](const Frame &f) { this->handle_frame_(f); };
  t_->on_sent = [this](const TxResult &r) { this->handle_sent_(r); };
}

const char *Session::state_name() const {
  switch (state_) {
    case S_IDLE:
      return "idle";
    case S_REGISTER:
      return "registering";
    case S_SUBSCRIBE:
    case S_SUBSCRIBE_SETTLE:
      return "subscribing";
    case S_IDENTITY:
      return "identifying";
    case S_DISCOVERY:
    case S_DISCOVERY_SETTLE:
      return "discovering";
    case S_MEASURE:
      return "measuring";
    case S_RUNNING:
      return "running";
    case S_FAILED:
      return "failed";
  }
  return "?";
}

void Session::enter_(State s, uint32_t now) {
  state_ = s;
  state_since_ = now;
}

void Session::start(uint32_t now) {
  now_ = now;
  registered_ = false;
  assigned_ = DEV_APP_DEFAULT;
  asked_.clear();
  acked_.clear();
  disc_round_ = 0;
  last_frame_ = now;
  start_time_ = now;
  enter_(S_REGISTER, now);
  LOGD("session: registering");
  t_->queue_frame(build_register_frame(assigned_), false, 0, DEV_BROADCAST);
}

void Session::stop() {
  if (state_ != S_IDLE && state_ != S_FAILED)
    LOGD("session: stopped in state %s", state_name());
  state_ = S_IDLE;
}

void Session::fail_(const char *reason) {
  if (state_ == S_FAILED || state_ == S_IDLE)
    return;
  LOGW("session failed (%s): %s", state_name(), reason);
  state_ = S_FAILED;
  if (on_failed)
    on_failed(reason);
}

void Session::loop(uint32_t now) {
  now_ = now;
  if (state_ == S_IDLE || state_ == S_FAILED)
    return;
  if (t_->invalidated()) {
    fail_("transport gave up");
    return;
  }
  if (state_ != S_RUNNING && now - start_time_ > startup_timeout) {
    fail_("startup did not finish in time");
    return;
  }
  switch (state_) {
    case S_REGISTER:
      if (registered_) {
        LOGD("session: panel assigned address 0x%04X, subscribing", assigned_);
        for (auto &batch : TOPIC_BATCHES)
          t_->queue_frame(build_subscribe_frame(assigned_, batch), false, subscribe_gap, DEV_MSG_BROKER);
        enter_(S_SUBSCRIBE, now);
      } else if (now - state_since_ > register_timeout) {
        fail_("the panel assigned no address -- the link is up but carries nothing");
      }
      break;
    case S_SUBSCRIBE:
      if (t_->idle()) {
        wait_until_ = now + subscribe_settle;
        enter_(S_SUBSCRIBE_SETTLE, now);
      }
      break;
    case S_SUBSCRIBE_SETTLE:
      if (time_reached(now, wait_until_)) {
        const uint32_t ts = unix_time ? unix_time() : 0;
        if (ts != 0) {
          t_->queue_frame(build_write_frame(assigned_, DEV_PANEL, "SystemTime", "Time", CborValue::make_int(ts)), false,
                          identity_gap, DEV_PANEL);
          t_->queue_frame(build_write_frame(assigned_, DEV_PANEL, "SystemTime", "Lot", CborValue::make_int(0)), false,
                          identity_gap, DEV_PANEL);
        } else {
          LOGD("session: no valid time yet, not sending SystemTime");
        }
        t_->queue_frame(build_write_frame(assigned_, DEV_PANEL, "MobileIdentity", "UserName",
                                          CborValue::make_text(identity.user_name)),
                        false, identity_gap, DEV_PANEL);
        t_->queue_frame(
            build_write_frame(assigned_, DEV_PANEL, "MobileIdentity", "Muid", CborValue::make_text(identity.muid)),
            false, identity_gap, DEV_PANEL);
        t_->queue_frame(
            build_write_frame(assigned_, DEV_PANEL, "MobileIdentity", "Uuid", CborValue::make_text(identity.uuid)),
            false, identity_gap, DEV_PANEL);
        t_->queue_frame(build_last_message_frame(assigned_), false, identity_gap, DEV_PANEL);
        enter_(S_IDENTITY, now);
      }
      break;
    case S_IDENTITY:
      if (t_->idle()) {
        LOGD("session: parameter discovery");
        t_->queue_frame(build_param_disc_frame(assigned_, DEV_BROADCAST), true, disc_gap, DEV_BROADCAST);
        disc_round_ = 0;
        discovery_round_();
        enter_(S_DISCOVERY, now);
      }
      break;
    case S_DISCOVERY:
      if (t_->idle()) {
        wait_until_ = now + disc_settle;
        enter_(S_DISCOVERY_SETTLE, now);
      }
      break;
    case S_DISCOVERY_SETTLE:
      if (!time_reached(now, wait_until_))
        break;
      disc_round_++;
      if (disc_round_ < 2 && discovery_round_()) {
        enter_(S_DISCOVERY, now);
        break;
      }
      LOGI("session: parameter discovery asked %u device(s), %u acknowledged", static_cast<unsigned>(asked_.size()),
           static_cast<unsigned>(acked_.size()));
      if (!asked_.empty() && acked_.empty()) {
        fail_("no device acknowledged parameter discovery -- the link carries nothing");
        break;
      }
      measure_(now);
      enter_(S_MEASURE, now);
      break;
    case S_MEASURE:
      if (t_->idle()) {
        enter_(S_RUNNING, now);
        next_measure_ = now + measure_interval;
        LOGI("session: running");
        if (on_ready)
          on_ready();
      }
      break;
    case S_RUNNING:
      if (now - last_frame_ > stall_timeout) {
        fail_("no data from the panel for too long -- link is stale");
        break;
      }
      if (time_reached(now, next_measure_) && t_->idle()) {
        next_measure_ = now + measure_interval;
        measure_(now);
      }
      break;
    default:
      break;
  }
}

bool Session::discovery_round_() {
  std::set<uint16_t> targets = device_seed();
  for (uint16_t a : bus.addresses())
    targets.insert(a);
  bool any = false;
  for (uint16_t a : targets) {
    if (asked_.count(a) || a == assigned_ || a == DEV_BROADCAST || a == DEV_MSG_BROKER)
      continue;
    asked_.insert(a);
    t_->queue_frame(build_param_disc_frame(assigned_, a), true, disc_gap, a, TAG_DISC);
    any = true;
  }
  return any;
}

void Session::request_measurements() {
  if (state_ == S_RUNNING)
    measure_(now_);
}

void Session::measure_(uint32_t now) {
  static const char *const TOPICS[2] = {"FreshWater", "GreyWater"};
  for (uint8_t k = 0; k < 2; k++) {
    for (uint16_t dest : bus.publishers(TOPICS[k], "Level")) {
      auto key = std::make_pair(dest, k);
      auto it = misses_.find(key);
      if (it != misses_.end()) {
        if (static_cast<int32_t>(bus.devices[dest].last_seen - it->second.seen_at) > 0) {
          misses_.erase(it);
        } else if (it->second.count >= 3) {
          continue;
        }
      }
      LOGV("session: asking 0x%04X for a fresh %s measurement", dest, TOPICS[k]);
      t_->queue_frame(build_write_frame(assigned_, dest, TOPICS[k], "MeasureRequest", CborValue::make_int(1)), true,
                      disc_gap, dest, k == 0 ? TAG_MEASURE_FRESH : TAG_MEASURE_GREY);
    }
  }
  (void) now;
}

void Session::handle_sent_(const TxResult &r) {
  if (r.tag == TAG_DISC && r.success)
    acked_.insert(r.dest);
  if (r.tag == TAG_MEASURE_FRESH || r.tag == TAG_MEASURE_GREY) {
    auto key = std::make_pair(r.dest, static_cast<uint8_t>(r.tag == TAG_MEASURE_FRESH ? 0 : 1));
    if (r.success) {
      misses_.erase(key);
    } else {
      Miss &m = misses_[key];
      m.count++;
      m.seen_at = bus.devices[r.dest].last_seen;
      if (m.count == 3)
        LOGW("0x%04X did not answer 3 measurement requests; its tank level will not refresh until it publishes "
             "again",
             r.dest);
    }
  }
}

void Session::handle_frame_(const Frame &f) {
  last_frame_ = now_;
  if (f.src != DEV_BROADCAST && f.src != DEV_MSG_BROKER && f.src != assigned_)
    bus.note_seen(f.src, now_);
  if (!f.has_cbor || !f.has_sub)
    return;
  const CborValue &c = f.cbor;

  // Registration response -> our bus address.
  if (f.control == CTRL_REGISTRATION && f.sub_type == 0x02) {
    const CborValue *addr = c.get("addr");
    if (addr != nullptr && addr->type == CborValue::INT && addr->i != 0) {
      assigned_ = static_cast<uint16_t>(addr->i);
      registered_ = true;
    }
    return;
  }
  if (f.control != CTRL_MBP)
    return;
  if (log_raw_on_) {
    if (static_cast<int32_t>(now_ - log_raw_until_) > 0) {
      log_raw_on_ = false;
      LOGI("raw: logging finished");
    } else {
      LOGI("raw 0x%04X->0x%04X sub=0x%02X corr=%u %s", f.src, f.dest, f.sub_type, static_cast<unsigned>(f.corr_id),
           c.to_string().c_str());
    }
  }

  auto file = [this, &f](const CborValue *tn, const CborValue *pn, const CborValue *v) {
    if (tn == nullptr || pn == nullptr || v == nullptr || tn->type != CborValue::TEXT || pn->type != CborValue::TEXT ||
        v->type == CborValue::NUL)
      return;
    const bool changed = bus.update(f.src, tn->s, pn->s, *v, now_);
    if (log_params && changed)
      LOGD("0x%04X %s.%s = %s", f.src, tn->s.c_str(), pn->s.c_str(), v->to_string().c_str());
    if (changed && on_update)
      on_update(f.src, tn->s, pn->s, *v);
  };

  if (f.sub_type == MBP_INFO) {
    file(c.get("tn"), c.get("pn"), c.get("v"));
  } else if (f.sub_type == MBP_PARAM_DISC_RESP) {
    const CborValue *topics = c.get("topics");
    if (topics == nullptr || topics->type != CborValue::ARRAY)
      return;
    for (auto &topic : topics->items) {
      const CborValue *tn = topic.get("tn");
      const CborValue *params = topic.get("parameters");
      if (params == nullptr || params->type != CborValue::ARRAY)
        continue;
      for (auto &p : params->items)
        file(tn, p.get("pn"), p.get("v"));
    }
  }
}

uint16_t Session::resolve_dest(const std::string &topic, const std::string &param) const {
  // The panel relays the room mode to whichever appliance serves the room.
  if (topic == "RoomClimate")
    return DEV_PANEL;
  uint16_t a = bus.publisher(topic, param);
  if (a != 0)
    return a;
  // Fall back to whoever publishes anything in that topic.
  const std::string prefix = topic + ".";
  uint32_t best = 0;
  for (auto &d : bus.devices) {
    for (auto &p : d.second.params) {
      if (p.first.compare(0, prefix.size(), prefix) == 0 && (a == 0 || static_cast<int32_t>(p.second.updated - best) > 0)) {
        a = d.first;
        best = p.second.updated;
      }
    }
  }
  return a;
}

bool Session::write(const std::string &topic, const std::string &param, const CborValue &value, uint16_t dest) {
  if (state_ != S_RUNNING) {
    LOGW("write %s.%s ignored: not connected to the panel", topic.c_str(), param.c_str());
    return false;
  }
  if (dest == 0)
    dest = resolve_dest(topic, param);
  if (dest == 0) {
    LOGW("write %s.%s ignored: no device on the bus publishes it (set 'address:')", topic.c_str(), param.c_str());
    return false;
  }
  LOGI("write %s.%s = %s -> 0x%04X", topic.c_str(), param.c_str(), value.to_string().c_str(), dest);
  t_->queue_frame(build_write_frame(assigned_, dest, topic, param, value), false, 0, dest);
  return true;
}

}  // namespace truma_inetx_proto
