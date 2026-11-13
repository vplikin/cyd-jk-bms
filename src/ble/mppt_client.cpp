// EPEVER MPPT via the HN_xxxx BLE gateway: transparent Modbus RTU over GATT.
//   service 0x2B00, write 0x2B11 (no response), notify 0x2B10.
//
// Mode: one persistent link + a slow poll cycle. Reconnect/discovery only after
// the link really died — far less radio churn than connect-per-poll, and
// nothing is ever written to flash (no bonding, no NVS).
//
// Runs on the "jk_ble" task. The notify callback (NimBLE host task) only fills
// a small RX buffer; parsing happens here.
#include "mppt_client.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>

#include "battery_state.h"
#include "config.h"
#include "mppt_state.h"

#if MPPT_ENABLE

namespace {

constexpr uint32_t kRssiEveryMs = 30000;
constexpr uint32_t kReconnectMinMs = 15000;
constexpr uint32_t kReconnectMaxMs = 120000;

struct Req {
  uint16_t reg;
  uint8_t n;
};
// Cycle order: live power first, then slow-changing values.
constexpr Req kReqs[] = {{0x3100, 8}, {0x3200, 2}, {0x3110, 2}, {0x330C, 2}, {0x3312, 2}};
constexpr uint8_t kReqCount = sizeof(kReqs) / sizeof(kReqs[0]);

class Cbs : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *) override { Serial.println("MPPT: connected"); }
  void onDisconnect(NimBLEClient *) override;
} g_cbs;

NimBLEClient *g_client = nullptr;
NimBLERemoteCharacteristic *g_wr = nullptr;
NimBLEAddress g_target;

bool g_connected = false;
volatile bool g_disc_event = false;
uint32_t g_next_try = 0;
uint32_t g_backoff = kReconnectMinMs;
uint32_t g_poll_ms = MPPT_POLL_MS;
uint32_t g_next_cycle = 0;
uint32_t g_last_rssi = 0;

// Request in flight
int8_t g_req = -1;  // index into kReqs, -1 = idle
uint32_t g_req_sent = 0;
bool g_cycle_ok = false;  // at least one valid reply in this cycle
uint8_t g_dead_cycles = 0;

// RX (written by the host task)
uint8_t g_rx[64];
volatile uint8_t g_rx_len = 0;
volatile bool g_rx_done = false;
volatile uint8_t g_rx_want = 0;  // full frame length expected (0 = not armed)

MpptData g_acc;  // accumulated across one cycle

inline bool elapsed(uint32_t now, uint32_t since, uint32_t ms) {
  return (int32_t)(now - since) >= (int32_t)ms;
}

uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
  }
  return c;
}

void set_phase(const char *p) {
  battery_state_lock();
  MpptState &s = mppt_state();
  strncpy(s.phase, p, sizeof(s.phase) - 1);
  s.phase[sizeof(s.phase) - 1] = 0;
  battery_state_unlock();
}

void Cbs::onDisconnect(NimBLEClient *) {
  Serial.println("MPPT: disconnected");
  g_disc_event = true;
}

void on_notify(const uint8_t *p, size_t len) {
  if (g_rx_done || !g_rx_want) return;
  uint8_t l = g_rx_len;
  // A reply starts with our slave address; skip anything before it.
  size_t i = 0;
  if (l == 0)
    while (i < len && p[i] != MPPT_MODBUS_ADDR) i++;
  for (; i < len && l < sizeof(g_rx); i++) g_rx[l++] = p[i];
  g_rx_len = l;
  if (l >= 2 && (g_rx[1] & 0x80) && l >= 5) g_rx_done = true;  // exception frame
  else if (l >= g_rx_want) g_rx_done = true;
}

bool send_req(uint8_t idx) {
  const Req &r = kReqs[idx];
  uint8_t f[8] = {MPPT_MODBUS_ADDR, 0x04, (uint8_t)(r.reg >> 8), (uint8_t)r.reg, 0, r.n};
  const uint16_t c = crc16(f, 6);
  f[6] = c & 0xFF;
  f[7] = c >> 8;
  g_rx_done = false;
  g_rx_len = 0;
  g_rx_want = 5 + 2 * r.n;
  g_req = idx;
  g_req_sent = millis();
  return g_wr && g_wr->writeValue(f, sizeof(f), false);
}

inline uint16_t reg(const uint8_t *d, int i) { return (uint16_t)(d[3 + 2 * i] << 8 | d[4 + 2 * i]); }
inline float s16(uint16_t v) { return (int16_t)v / 100.0f; }
inline float u32(uint16_t lo, uint16_t hi) { return ((uint32_t)hi << 16 | lo) / 100.0f; }

// Validate the RX frame for request idx and fold it into g_acc.
bool parse(uint8_t idx) {
  const Req &r = kReqs[idx];
  const uint8_t n = g_rx_len;
  const uint8_t want = 5 + 2 * r.n;
  if (n < want || g_rx[0] != MPPT_MODBUS_ADDR || g_rx[1] != 0x04 || g_rx[2] != 2 * r.n) return false;
  if (crc16(g_rx, want - 2) != (uint16_t)(g_rx[want - 2] | g_rx[want - 1] << 8)) return false;
  const uint8_t *d = g_rx;
  switch (r.reg) {
    case 0x3100:
      g_acc.pv_v = reg(d, 0) / 100.0f;
      g_acc.pv_a = reg(d, 1) / 100.0f;
      g_acc.pv_w = u32(reg(d, 2), reg(d, 3));
      g_acc.bat_v = reg(d, 4) / 100.0f;
      g_acc.bat_a = reg(d, 5) / 100.0f;
      g_acc.bat_w = u32(reg(d, 6), reg(d, 7));
      break;
    case 0x3110:
      g_acc.temp_bat_c = s16(reg(d, 0));
      g_acc.temp_dev_c = s16(reg(d, 1));
      g_acc.has_temps = true;
      break;
    case 0x3200:
      g_acc.bat_status = reg(d, 0);
      g_acc.chg_status = reg(d, 1);
      g_acc.has_status = true;
      break;
    case 0x330C:
      g_acc.gen_today_kwh = u32(reg(d, 0), reg(d, 1));
      g_acc.has_energy = true;
      break;
    case 0x3312:
      g_acc.gen_total_kwh = u32(reg(d, 0), reg(d, 1));
      break;
  }
  return true;
}

void publish_live() {
  const uint32_t now = millis();
  battery_state_lock();
  MpptState &s = mppt_state();
  s.d = g_acc;
  s.has_data = true;
  s.last_update_ms = now;
  s.ok_count++;
  strcpy(s.phase, "DATA");
  battery_state_unlock();
}

bool connect_link() {
  if (!g_client) {
    g_client = NimBLEDevice::createClient();
    if (!g_client) return false;
    g_client->setClientCallbacks(&g_cbs, false);
    g_client->setConnectionParams(BLE_CONN_ITVL_MIN, BLE_CONN_ITVL_MAX, 0, BLE_CONN_TIMEOUT,
                                  BLE_SCAN_ITVL, BLE_SCAN_WINDOW);
    g_client->setConnectTimeout(BLE_CONNECT_TIMEOUT_S);
  }
  set_phase("CONNECT");
  g_disc_event = false;
  Serial.printf("MPPT: connecting %s\n", g_target.toString().c_str());
  if (!g_client->connect(g_target, true)) {
    const uint8_t t = g_target.getType() == BLE_ADDR_PUBLIC ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    g_target = NimBLEAddress(g_target.toString(), t);
    Serial.println("MPPT: connect failed (phone app holding the gateway?)");
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(200));
  NimBLERemoteService *svc = g_client->getService(NimBLEUUID((uint16_t)0x2B00));
  NimBLERemoteCharacteristic *nt = svc ? svc->getCharacteristic(NimBLEUUID((uint16_t)0x2B10)) : nullptr;
  g_wr = svc ? svc->getCharacteristic(NimBLEUUID((uint16_t)0x2B11)) : nullptr;
  if (!nt || !g_wr) {
    Serial.println("MPPT: 2B00/2B10/2B11 missing");
    g_client->disconnect();
    g_wr = nullptr;
    return false;
  }
  auto cb = [](NimBLERemoteCharacteristic *, uint8_t *p, size_t l, bool) { on_notify(p, l); };
  if (!nt->subscribe(true, cb, true) || !g_client->isConnected()) {
    Serial.println("MPPT: subscribe failed");
    g_client->disconnect();
    g_wr = nullptr;
    return false;
  }
  g_client->updateConnParams(BLE_CONN_ITVL_MIN, BLE_CONN_ITVL_MAX, 0, BLE_CONN_TIMEOUT);
  const int rssi = g_client->getRssi();
  Serial.printf("MPPT: link RSSI=%d\n", rssi);

  g_connected = true;
  g_backoff = kReconnectMinMs;
  g_req = -1;
  g_dead_cycles = 0;
  g_next_cycle = millis() + 300;
  g_last_rssi = millis();
  battery_state_lock();
  MpptState &s = mppt_state();
  s.connected = true;
  if (rssi != 0) s.rssi_dbm = rssi;
  strcpy(s.phase, "LINK");
  battery_state_unlock();
  return true;
}

void drop_link(uint32_t now) {
  g_connected = false;
  g_wr = nullptr;
  g_req = -1;
  g_rx_want = 0;
  battery_state_lock();
  mppt_state().connected = false;
  strcpy(mppt_state().phase, "FAIL");
  battery_state_unlock();
  g_next_try = now + g_backoff;
  g_backoff = g_backoff * 2 > kReconnectMaxMs ? kReconnectMaxMs : g_backoff * 2;
}

void end_cycle(uint32_t now) {
  g_req = -1;
  g_rx_want = 0;
  g_next_cycle = now + g_poll_ms;
  if (g_cycle_ok) {
    g_dead_cycles = 0;
    return;
  }
  battery_state_lock();
  mppt_state().err_count++;
  strcpy(mppt_state().phase, "NOREPLY");
  battery_state_unlock();
  if (++g_dead_cycles >= MPPT_MAX_DEAD_CYCLES) {
    Serial.println("MPPT: no replies — reconnect");
    g_dead_cycles = 0;
    g_client->disconnect();  // → onDisconnect → drop_link
  }
}

}  // namespace

void mppt_begin(uint32_t now) {
  if (!MPPT_MAC[0]) {
    set_phase("OFF");
    return;
  }
  g_target = NimBLEAddress(std::string(MPPT_MAC), BLE_ADDR_PUBLIC);
  g_next_try = now + 20000;  // after both BMS links had their first go
  set_phase("WAIT");
}

void mppt_manage(uint32_t now) {
  if (g_disc_event) {
    g_disc_event = false;
    if (g_connected) drop_link(now);
  }

  if (!g_connected) {
    if (!MPPT_MAC[0] || !elapsed(now, g_next_try, 0)) return;
    if (NimBLEDevice::getScan()->isScanning()) return;  // JK scan owns the radio now
    if (!connect_link()) {
      set_phase("FAIL");
      g_next_try = millis() + g_backoff;
      g_backoff = g_backoff * 2 > kReconnectMaxMs ? kReconnectMaxMs : g_backoff * 2;
    }
    return;
  }

  if (elapsed(now, g_last_rssi, kRssiEveryMs)) {
    g_last_rssi = now;
    const int rssi = g_client->getRssi();
    if (rssi != 0) {
      battery_state_lock();
      mppt_state().rssi_dbm = rssi;
      battery_state_unlock();
    }
  }

  if (g_req < 0) {
    if (!elapsed(now, g_next_cycle, 0)) return;
    g_cycle_ok = false;
    if (!send_req(0)) end_cycle(now);
    return;
  }

  const bool done = g_rx_done;
  if (!done && !elapsed(now, g_req_sent, MPPT_REPLY_TIMEOUT_MS)) return;

  const uint8_t idx = (uint8_t)g_req;
  if (done && parse(idx)) {
    g_cycle_ok = true;
    if (idx == 0) publish_live();  // power is on screen as soon as it arrives
  } else if (idx == 0) {
    end_cycle(now);  // no live block → skip the rest of this cycle
    return;
  }
  if (idx + 1 < kReqCount) {
    if (!send_req(idx + 1)) end_cycle(now);
    return;
  }
  publish_live();  // full cycle incl. status/temps/energy
  end_cycle(now);
}

void mppt_set_sleep(bool sleep) {
  g_poll_ms = sleep ? MPPT_POLL_SLEEP_MS : MPPT_POLL_MS;
  if (!sleep) g_next_cycle = millis();
}

#else
void mppt_begin(uint32_t) {}
void mppt_manage(uint32_t) {}
void mppt_set_sleep(bool) {}
#endif
