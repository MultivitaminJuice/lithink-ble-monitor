# lithink-ble-monitor

Standalone **ESP32** firmware (Arduino, **no ESPHome**) that reads one or more
**JBD / Jiabaida** LiFePO4 batteries — e.g. **Lithink 12 V 334 Ah** — over
**Bluetooth LE** and serves a small **web UI + JSON API** over WiFi.

- The battery-reading core ([`jbd_bms.h`](jbd_bms.h)) is framework-free C++ and
  is meant to be **lifted into another product** (another firmware or controller).
- The display device (phone/laptop) only needs a browser — **no BLE on the
  client side**. The ESP32 is the BLE reader.

![Web UI — two batteries with live data and charge/discharge control](docs/web-ui.png)

---

## What you get

- Reads **voltage, current, power, SOC, remaining/nominal capacity, cycles**
- **Per-cell voltages** + min / max / delta, **temperatures**, balancing
- **Protection flags**, charge/discharge MOSFET status
- Multiple batteries on one ESP32 (each its own BLE connection)
- Web UI at `http://<hostname>.local` and a JSON API at `/api/data`
- Optional **charge/discharge control** (off by default)
- Optional **wireless firmware updates** (ArduinoOTA) as a separable module

### Optional modules

Two features are isolated so an integrator who only needs the battery reader —
not the UI or the updater — can drop them:

| Module | File | Flag | Default |
|--------|------|------|---------|
| Web UI + JSON API | `web_page.h` + handlers in the `.ino` | — | on |
| Network updates (OTA) | [`ota.h`](ota.h) | `ENABLE_OTA` | on |
| Charge/discharge control | in `jbd_bms.h` / web UI | `ENABLE_CONTROL` | off |

The reusable core — [`jbd_bms.h`](jbd_bms.h) — has none of these and depends only
on NimBLE.

---

## Requirements

- Any classic **ESP32** board (`esp32dev`)
- **ESP32 Arduino core 2.0.x** (IDF 4.4) — PlatformIO pins `espressif32 @ 6.9.0`
- **NimBLE-Arduino 1.4.x** library  (1.4.x needs core 2.x — verified booting on real hardware)
- The ESP32 must be **physically close to the batteries** (BLE range, metal
  enclosures attenuate a lot) **and** on a WiFi it can reach reliably.

---

## Build & flash

### Config first
```bash
cp config.example.h config.h      # then edit config.h
```
Fill in your WiFi SSID/password and the battery MAC addresses. `config.h` is
git-ignored so your WiFi password never lands in the repo. Find the MACs with a
BLE scanner (nRF Connect) or from the serial log on first boot.

### Arduino IDE
1. Install **ESP32 board support** — use a **2.0.x** release (Boards Manager →
   “esp32” → pick 2.0.17). NimBLE-Arduino 1.4.x aborts at boot on the 3.x core.
2. Library Manager → install **NimBLE-Arduino** (use a **1.4.x** release).
3. Tools → **Partition Scheme** → **“Minimal SPIFFS (1.9MB APP with OTA)”**
   (needed for the OTA module and to fit the BLE + WiFi stack).
4. Open this folder as a sketch, select your ESP32 board, **Upload** (USB).

### PlatformIO
```bash
pio run -t upload        # NimBLE + partition scheme pinned in platformio.ini
pio device monitor
```

Open `http://lithink-bridge.local` (or the IP from the serial log) on a device
on the **same** WiFi.

### Wireless updates (OTA)

The **first** flash must be over **USB**. After that, with `ENABLE_OTA = 1`,
there are two wireless paths:

**1. Web upload (recommended)** — a browser/`curl` POST to `/update`. This goes
in the same direction as the web UI (client → ESP), so it works even across
**routed / Tailscale / firewalled** networks:

```bash
curl -u admin:<OTA_PASSWORD> -F "fw=@.pio/build/esp32dev/firmware.bin" http://<host>/update
# or open http://<host>/update in a browser
```

**2. ArduinoOTA (espota)** — only on the **same L2 network** as the device, and
it needs the host reachable *from* the ESP (reverse connection), so it fails on
Tailscale/routed setups and when the host firewall blocks the inbound port:

```bash
pio run -t upload --upload-port lithink-bridge.local --upload-flags --auth=<OTA_PASSWORD>
# Arduino IDE: Tools -> Port -> network port "lithink-bridge"
```

> Moving off an existing ESPHome install: ESPHome's OTA can't push this Arduino
> firmware, so do the **one** switch-over flash via USB. From then on it's
> wireless (web upload is the robust option).

---

## JSON API

`GET /api/data` → one object per battery:

```json
{ "batteries": [ {
  "name":"Akku Bank","connected":true,"valid":true,"age_s":3,
  "voltage":13.29,"current":0.00,"power":0,"soc":99,
  "remaining_ah":331.30,"nominal_ah":334.00,"cycles":1,
  "cell_count":4,"cell_delta":0.002,"cells":[3.323,3.324,3.325,3.323],
  "temps":[22.4,20.6],
  "charging_fet":true,"discharging_fet":true,"balancing":false,"protection":0
} ] }
```

`POST /api/control?bat=<i>&what=charge|discharge&on=0|1` — only active when built
with `ENABLE_CONTROL = 1`.

---

## JBD BLE protocol (reference for integration)

GATT layout (standard JBD / “xiaoxiang”):

| | UUID | direction |
|---|---|---|
| Service | `0xFF00` | |
| Notify  | `0xFF01` | BMS → host (data frames) |
| Write   | `0xFF02` | host → BMS (commands) |

**Framing**
```
Request  : DD A5 <reg> 00 <crc_hi> <crc_lo> 77
Response : DD <func> <status> <len> <data..> <crc_hi> <crc_lo> 77
Checksum : 16-bit, 0x0000 - sum(bytes from <status/reg> through end of <data>)
```
Read basic info → `DD A5 03 00 FF FD 77`  ·  read cells → `DD A5 04 00 FF FC 77`.
Responses can span several BLE notifications — reassemble until the announced
length (`4 + len + 3`) is complete, then verify trailing `0x77`, CRC and
`status == 0`.

**Basic info (func `0x03`)** — byte offsets into `<data>`:

| off | field | type | scale |
|----|-------|------|-------|
| 0  | total voltage | u16 BE | ×0.01 V |
| 2  | current | i16 BE | ×scale A (signed) |
| 4  | remaining capacity | u16 BE | ×scale Ah |
| 6  | nominal capacity | u16 BE | ×scale Ah |
| 8  | cycles | u16 BE | — |
| 12/14 | balance bitmask | u16+u16 | cells 1–16 / 17–32 |
| 16 | protection bitmask | u16 BE | 0 = OK |
| 19 | SOC | u8 | % |
| 20 | FET status | u8 | bit0 charge, bit1 discharge; **bit7 = capacity/current scale** (set → 0.1, else 0.01) |
| 21 | cell count | u8 | — |
| 22 | temp-sensor count | u8 | — |
| 23+ | temperatures | u16 BE each | (raw − 2731) × 0.1 °C |

**Cell info (func `0x04`)**: `len/2` cells, each `u16 BE × 0.001 V`.

**MOSFET control** (register `0xE1`, write `0x5A`): read the current FET byte
(basic info byte 20), set bit 0 (charge) / bit 1 (discharge), then write the
**inverted** low two bits (`1 = off`). See `changeMosfet_()` in `jbd_bms.h`.

---

## Known limitations

- **Two simultaneous BLE connections on one ESP32 are near the RF limit.**
  Reliable operation needs the ESP close to both batteries and good WiFi
  (shared 2.4 GHz radio). If connections flap, consider **one ESP per battery**.
- Data in the JSON is updated from a background task; a reader may briefly see a
  value mid-refresh. Fine for monitoring, not for safety interlocks.
- Control over a 300 A BMS from a browser is powerful — keep `ENABLE_CONTROL`
  off unless you mean it.

## License

MIT — see [LICENSE](LICENSE).
