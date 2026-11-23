// Host tests for the platform-independent core.
//   g++ -std=c++17 -I../components/truma_inetx test_core.cpp ../components/truma_inetx/truma_core.cpp -lcrypto
#include <openssl/aes.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "truma_core.h"

using namespace truma_inetx_proto;

static int g_fail = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
      g_fail++;                                                        \
    }                                                                  \
  } while (0)

static bool g_verbose = false;
static void log_fn(int level, const char *msg) {
  if (g_verbose || level <= 1)
    printf("    [%d] %s\n", level, msg);
}

static std::vector<uint8_t> hex(const char *h) {
  std::vector<uint8_t> out;
  while (*h && h[1]) {
    if (*h == ' ') {
      h++;
      continue;
    }
    unsigned v;
    sscanf(h, "%2x", &v);
    out.push_back(static_cast<uint8_t>(v));
    h += 2;
  }
  return out;
}

static void aes128(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  AES_KEY k;
  AES_set_encrypt_key(key, 128, &k);
  AES_encrypt(in, out, &k);
}

// ---------------------------------------------------------------------------
static void test_cbor() {
  printf("cbor\n");
  CborValue v;
  auto dec = [&](const char *h) {
    auto b = hex(h);
    return cbor_decode(b.data(), b.size(), v);
  };
  CHECK(dec("1903e8") && v.type == CborValue::INT && v.i == 1000);
  CHECK(dec("3903e7") && v.i == -1000);
  CHECK(dec("f93c00") && v.type == CborValue::FLOAT && v.f == 1.0);
  CHECK(dec("fb3ff199999999999a") && v.f == 1.1);
  CHECK(dec("6449455446") && v.type == CborValue::TEXT && v.s == "IETF");
  CHECK(dec("9f018202039f0405ffff") && v.type == CborValue::ARRAY && v.items.size() == 3 && v.items[2].items[1].i == 5);
  CHECK(dec("a26161016162820203") && v.get("b") && v.get("b")->items.size() == 2);
  CHECK(dec("bf61610161629f0203ffff") && v.get("a")->i == 1);
  CHECK(dec("f5") && v.type == CborValue::BOOL && v.i == 1);
  CHECK(dec("f6") && v.type == CborValue::NUL);
  CHECK(dec("c11a514b67b0") && v.i == 1363896240);  // tagged
  CHECK(!dec("1a0102"));                             // truncated
  CHECK(!dec("9b0000000100000000"));                 // absurd length

  CborWriter w;
  w.map(2);
  w.text("tn");
  w.text("AirHeating");
  w.text("v");
  w.integer(-299);
  CborValue r;
  CHECK(cbor_decode(w.bytes().data(), w.bytes().size(), r));
  CHECK(r.get("v")->i == -299 && r.get("tn")->s == "AirHeating");
  CHECK(r.to_string() == "{\"tn\":\"AirHeating\",\"v\":-299}");
}

static void test_frames() {
  printf("frames\n");
  // Python reference: build_register_frame(0x0500)
  auto f = build_register_frame(0x0500);
  CHECK(f == hex("ffff0005120001000000000000000000 0142a1627076820501"));
  Frame p;
  CHECK(parse_frame(f.data(), f.size(), p));
  CHECK(p.dest == 0xFFFF && p.src == 0x0500 && p.control == 1 && p.sub_type == 1 && p.corr_id == 0x42);
  CHECK(p.has_cbor && p.cbor.get("pv")->items.size() == 2);

  auto w = build_write_frame(0x0502, 0x0201, "AirHeating", "TgtTemp", CborValue::make_int(215));
  CHECK(parse_frame(w.data(), w.size(), p));
  CHECK(p.dest == 0x0201 && p.src == 0x0502 && p.control == CTRL_MBP && p.sub_type == MBP_WRITE);
  CHECK(p.cbor.get("v")->i == 215 && p.cbor.get("id")->i == 0);
  CHECK(p.pkt_size == w.size() - 7);

  auto d = build_param_disc_frame(0x0502, 0x0405);
  CHECK(d.size() == 18 && d[16] == MBP_PARAM_DISC);
  CHECK(parse_frame(d.data(), d.size(), p) && !p.has_cbor);
  CHECK(!parse_frame(d.data(), 15, p));
}

static void test_rpa() {
  printf("rpa\n");
  // Core spec Vol 3 Part H, D.7: k = ec0234a357c8ad05341010a60a397d9b,
  // r = 708194 -> ah = 0dfbaa. RPA (MSB first) 70:81:94:0d:fb:aa.
  auto key_msb = hex("ec0234a357c8ad05341010a60a397d9b");
  uint8_t irk_lsb[16];
  for (int i = 0; i < 16; i++)
    irk_lsb[i] = key_msb[15 - i];
  uint8_t addr[6] = {0x70, 0x81, 0x94, 0x0d, 0xfb, 0xaa};
  CHECK(rpa_matches(addr, irk_lsb, aes128));
  CHECK(rpa_matches(addr, key_msb.data(), aes128));  // either byte order
  addr[5] ^= 1;
  CHECK(!rpa_matches(addr, irk_lsb, aes128));
  uint8_t pub[6] = {0x30, 0x81, 0x94, 0x0d, 0xfb, 0xaa};  // not an RPA
  CHECK(!rpa_matches(pub, irk_lsb, aes128));
}

// ---------------------------------------------------------------------------
// A simulated panel behind a simulated GATT link.
struct Sim : TransportIO {
  uint32_t now = 0;
  uint16_t mtu = 23;
  struct Pending {
    uint32_t at;
    std::function<void()> fn;
  };
  std::vector<Pending> pending;
  Transport *t = nullptr;

  // panel state
  size_t expect_len = 0;
  std::vector<uint8_t> data_in;
  std::set<uint16_t> present{DEV_PANEL, DEV_HEATER, 0x0405};
  std::map<uint16_t, std::vector<std::tuple<std::string, std::string, int>>> params;
  std::vector<Frame> received;
  int confirms = 0, acks_from_host = 0;
  bool refuse_writes = false;
  bool silent = false;  // never answers registration
  uint16_t assign = 0x0502;

  Sim() {
    params[DEV_PANEL] = {{"RoomClimate", "Mode", 0}, {"RoomClimate", "TgtTemp", 220}};
    params[DEV_HEATER] = {{"AirHeating", "Temp", 198}, {"AirHeating", "TgtTemp", 200}, {"AirHeating", "Active", 0}};
    params[0x0405] = {{"FreshWater", "Level", 50}, {"VBat", "Voltage", 128}};
  }

  void after(uint32_t ms, std::function<void()> fn) { pending.push_back({now + ms, std::move(fn)}); }

  bool gatt_write(Channel ch, const uint8_t *d, size_t len, bool rsp) override {
    std::vector<uint8_t> data(d, d + len);
    if (ch == Channel::DATA_W)
      assert(len <= static_cast<size_t>(mtu - 3));
    after(5, [this, ch, data, rsp]() {
      this->panel_rx(ch, data);
      (void) rsp;
      t->on_write_complete(true, now);
    });
    return true;
  }

  void notify_cmd(std::vector<uint8_t> b) {
    after(10, [this, b]() { t->on_cmd_notify(b.data(), b.size(), now); });
  }
  uint32_t tx_free_at = 0;  // the panel sends one message at a time
  void send_frame(const std::vector<uint8_t> &f) {
    uint32_t start = std::max(now + 10, tx_free_at);
    std::vector<uint8_t> ann{TRANSPORT_MSG_ANNOUNCE, static_cast<uint8_t>(f.size() & 0xFF),
                             static_cast<uint8_t>(f.size() >> 8)};
    pending.push_back({start, [this, ann]() { t->on_cmd_notify(ann.data(), ann.size(), now); }});
    size_t chunk = mtu - 3;
    uint32_t at = start + 10;
    for (size_t off = 0; off < f.size(); off += chunk) {
      std::vector<uint8_t> part(f.begin() + off, f.begin() + std::min(f.size(), off + chunk));
      pending.push_back({at, [this, part]() { t->on_data_notify(part.data(), part.size(), now); }});
      at += 2;
    }
    tx_free_at = at + 5;
  }
  std::vector<uint8_t> disc_response(uint16_t src) {
    // {"topics":[{"tn":T,"parameters":[{"pn":P,"v":V},...]},...]}
    std::map<std::string, std::vector<std::pair<std::string, int>>> by_topic;
    for (auto &p : params[src])
      by_topic[std::get<0>(p)].push_back({std::get<1>(p), std::get<2>(p)});
    CborWriter w;
    w.map(1);
    w.text("topics");
    w.array(by_topic.size());
    for (auto &t : by_topic) {
      w.map(2);
      w.text("tn");
      w.text(t.first);
      w.text("parameters");
      w.array(t.second.size());
      for (auto &p : t.second) {
        w.map(3);
        w.text("pn");
        w.text(p.first);
        w.text("v");
        w.integer(p.second);
        w.text("type");
        w.uint(1);
      }
    }
    return build_frame(assign, src, CTRL_MBP, MBP_PARAM_DISC_RESP, 0, w.bytes());
  }

  void panel_rx(Channel ch, const std::vector<uint8_t> &d) {
    if (ch == Channel::CMD) {
      if (d.size() == 3 && d[0] == TRANSPORT_INIT) {
        expect_len = d[1] | (d[2] << 8);
        data_in.clear();
        notify_cmd({TRANSPORT_READY, 0x00});
      } else if (d.size() == 2 && d[0] == TRANSPORT_CONFIRM) {
        confirms++;
      } else if (d.size() == 2 && d[0] == TRANSPORT_ACK) {
        acks_from_host++;
      }
      return;
    }
    data_in.insert(data_in.end(), d.begin(), d.end());
    if (data_in.size() < expect_len)
      return;
    Frame f;
    assert(parse_frame(data_in.data(), data_in.size(), f));
    received.push_back(f);
    // Frames to absent devices are not acknowledged at all.
    bool deliverable = f.dest == DEV_BROADCAST || f.dest == DEV_MSG_BROKER || present.count(f.dest);
    if (!deliverable)
      return;
    if (f.control == CTRL_REGISTRATION) {
      notify_cmd({TRANSPORT_ACK, 0x01});
      if (!silent) {
        CborWriter w;
        w.map(1);
        w.text("addr");
        w.uint(assign);
        auto resp = build_frame(DEV_APP_DEFAULT, DEV_MSG_BROKER, CTRL_REGISTRATION, 0x02, 0x42, w.bytes());
        after(30, [this, resp]() { send_frame(resp); });
      }
      return;
    }
    if (f.sub_type == MBP_WRITE && refuse_writes && f.dest == DEV_HEATER) {
      notify_cmd({TRANSPORT_ACK, 0x00});
      return;
    }
    notify_cmd({TRANSPORT_ACK, 0x01});
    if (f.sub_type == MBP_PARAM_DISC) {
      std::vector<uint16_t> targets;
      if (f.dest == DEV_BROADCAST)
        targets = {DEV_PANEL, DEV_HEATER};  // only these answer a broadcast
      else
        targets = {f.dest};
      for (auto a : targets) {
        auto resp = disc_response(a);
        after(40, [this, resp]() { send_frame(resp); });
      }
    } else if (f.sub_type == MBP_WRITE && f.has_cbor && f.cbor.get("tn") && f.cbor.get("pn")) {
      // echo the new value as an info frame from the device
      CborWriter w;
      w.map(3);
      w.text("tn");
      w.text(f.cbor.get("tn")->s);
      w.text("pn");
      w.text(f.cbor.get("pn")->s);
      w.text("v");
      w.value(*f.cbor.get("v"));
      auto info = build_frame(assign, f.dest, CTRL_MBP, MBP_INFO, 0, w.bytes());
      after(50, [this, info]() { send_frame(info); });
    }
  }

  void push_info(uint16_t src, const char *tn, const char *pn, int v) {
    CborWriter w;
    w.map(3);
    w.text("tn");
    w.text(tn);
    w.text("pn");
    w.text(pn);
    w.text("v");
    w.integer(v);
    send_frame(build_frame(assign, src, CTRL_MBP, MBP_INFO, 0, w.bytes()));
  }

  void run(Session &s, uint32_t ms) {
    uint32_t end = now + ms;
    while (now < end) {
      now += 1;
      for (size_t i = 0; i < pending.size();) {
        if (pending[i].at <= now) {
          auto fn = std::move(pending[i].fn);
          pending.erase(pending.begin() + i);
          fn();
        } else {
          i++;
        }
      }
      t->loop(now);
      s.loop(now);
    }
  }
};

static void test_session_happy() {
  printf("session: happy path (MTU 23, fragmented frames)\n");
  Sim sim;
  Transport t(&sim);
  sim.t = &t;
  t.set_mtu(sim.mtu);
  Session s(&t);
  s.identity.muid = "11111111-2222-3333-4444-555555555555";
  s.identity.uuid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee";
  s.unix_time = []() { return 1789365720u; };
  bool ready = false, failed = false;
  std::map<std::string, int64_t> seen;
  s.on_ready = [&]() { ready = true; };
  s.on_failed = [&](const char *) { failed = true; };
  t.on_fatal = [&](const char *) { failed = true; };
  s.on_update = [&](uint16_t src, const std::string &tn, const std::string &pn, const CborValue &v) {
    char k[64];
    snprintf(k, sizeof(k), "%04X %s.%s", src, tn.c_str(), pn.c_str());
    seen[k] = v.i;
  };

  s.start(sim.now);
  sim.run(s, 110000);
  CHECK(ready);
  CHECK(!failed);
  CHECK(s.running());
  CHECK(s.assigned_addr() == 0x0502);
  CHECK(seen["0101 RoomClimate.Mode"] == 0);
  CHECK(seen["0201 AirHeating.Temp"] == 198);
  CHECK(seen["0405 FreshWater.Level"] == 50);
  CHECK(sim.confirms > 0 && sim.confirms == sim.acks_from_host);

  // frames the panel received, in order
  int subs = 0, ident = 0, disc = 0, measure = 0;
  for (auto &f : sim.received) {
    if (f.sub_type == MBP_SUBSCRIBE)
      subs++;
    if (f.sub_type == MBP_WRITE && f.dest == DEV_PANEL)
      ident++;
    if (f.sub_type == MBP_PARAM_DISC)
      disc++;
    if (f.sub_type == MBP_WRITE && f.has_cbor && f.cbor.get("pn") && f.cbor.get("pn")->s == "MeasureRequest")
      measure++;
  }
  CHECK(sim.received.front().control == CTRL_REGISTRATION);
  CHECK(subs == 4);
  CHECK(ident == 6);              // Time, Lot, UserName, Muid, Uuid, LastMessage
  CHECK(disc == 1 + 18);          // broadcast + seed (0x0101, 0x0201, 0x0401-8, 0x0601-8)
  CHECK(measure == 1);            // the one tank publisher
  for (auto &f : sim.received)
    if (f.sub_type != MBP_SUBSCRIBE && f.control != CTRL_REGISTRATION)
      CHECK(f.src == 0x0502);     // every frame after registration from the assigned address

  // A write is resolved to the publishing device and confirmed by an echo.
  CHECK(s.write("AirHeating", "TgtTemp", CborValue::make_int(215)));
  CHECK(s.write("RoomClimate", "Mode", CborValue::make_int(3)));
  sim.run(s, 3000);
  CHECK(seen["0201 AirHeating.TgtTemp"] == 215);
  CHECK(seen["0101 RoomClimate.Mode"] == 3);
  CHECK(!failed);

  // Unknown parameter: refused locally, nothing sent.
  size_t before = sim.received.size();
  CHECK(!s.write("Nope", "Nothing", CborValue::make_int(1)));
  sim.run(s, 500);
  CHECK(sim.received.size() == before);

  // Push updates keep the stall watchdog fed; silence trips it.
  for (int k = 0; k < 5; k++) {
    sim.push_info(DEV_HEATER, "AirHeating", "Temp", 200 + k);
    sim.run(s, 20000);
  }
  CHECK(!failed && s.running());
  CHECK(seen["0201 AirHeating.Temp"] == 204);
  // Periodic tank measurement requests happened meanwhile.
  int measure2 = 0;
  for (auto &f : sim.received)
    if (f.sub_type == MBP_WRITE && f.has_cbor && f.cbor.get("pn") && f.cbor.get("pn")->s == "MeasureRequest")
      measure2++;
  CHECK(measure2 >= 2);
  sim.present.erase(0x0405);  // nothing echoes any more
  sim.run(s, 95000);
  CHECK(failed);
}

static void test_session_silent() {
  printf("session: panel never assigns an address\n");
  Sim sim;
  sim.silent = true;
  Transport t(&sim);
  sim.t = &t;
  t.set_mtu(185);
  sim.mtu = 185;
  Session s(&t);
  bool failed = false;
  s.on_failed = [&](const char *) { failed = true; };
  s.start(sim.now);
  sim.run(s, 19000);
  CHECK(!failed);
  sim.run(s, 2000);
  CHECK(failed);
}

static void test_refused_write() {
  printf("session: refused write ends the link\n");
  Sim sim;
  Transport t(&sim);
  sim.t = &t;
  t.set_mtu(247);
  sim.mtu = 247;
  Session s(&t);
  bool fatal = false;
  t.on_fatal = [&](const char *) { fatal = true; };
  s.start(sim.now);
  sim.run(s, 110000);
  CHECK(s.running());
  sim.refuse_writes = true;
  s.write("AirHeating", "TgtTemp", CborValue::make_int(150));
  sim.run(s, 2000);
  CHECK(fatal);
  CHECK(t.invalidated());
  sim.run(s, 10);
  CHECK(s.state() == Session::S_FAILED);
}

static void test_stray_cmd() {
  printf("transport: stray Ready/DataAck outside a transfer are ignored\n");
  Sim sim;
  Transport t(&sim);
  sim.t = &t;
  t.set_mtu(247);
  sim.mtu = 247;
  Session s(&t);
  s.start(sim.now);
  sim.run(s, 110000);
  CHECK(s.running());
  // stray notifications like the van's bare f004 / 79
  uint8_t a[] = {0xF0, 0x04}, b[] = {0x79}, c[] = {0x81, 0x00};
  t.on_cmd_notify(a, 2, sim.now);
  t.on_cmd_notify(b, 1, sim.now);
  t.on_cmd_notify(c, 2, sim.now);
  CHECK(s.write("AirHeating", "TgtTemp", CborValue::make_int(180)));
  sim.run(s, 2000);
  CHECK(s.running() && !t.invalidated());
}

int main(int argc, char **) {
  g_verbose = argc > 1;
  set_log_fn(log_fn);
  test_cbor();
  test_frames();
  test_rpa();
  test_session_happy();
  test_session_silent();
  test_refused_write();
  test_stray_cmd();
  if (g_fail) {
    printf("%d check(s) FAILED\n", g_fail);
    return 1;
  }
  printf("all tests passed\n");
  return 0;
}
