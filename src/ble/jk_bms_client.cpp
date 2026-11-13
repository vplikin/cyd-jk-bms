// JK BMS BLE manager: up to JK_BMS_COUNT simultaneous links.
//
// Threads:
//  * "jk_ble" task (this file) — connects, sends init, watchdogs, RSSI. Blocking
//    NimBLE calls (connect / discovery) live here, so the UI loop never stalls.
//  * NimBLE host task — notify/scan/disconnect callbacks. They only touch
//    volatile slot fields and the shared BatteryState under its mutex.
#include "jk_bms_client.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <string.h>

#include <algorithm>
#include <vector>

#include "battery_state.h"
#include "config.h"
#include "jk_frame_decoder.h"
#include "mppt_client.h"

static const char *kServiceUuid = "ffe0";
static const char *kCharUuid = "ffe1";

// Take the next frame a bit earlier than the nominal period so that with the
// BMS's own ~1 s stream the effective interval stays ≈ BMS_POLL_MS, not +1 s.
static constexpr uint32_t kPollSlackMs = 600;
static constexpr uint32_t kRssiEveryMs = 10000;
static constexpr uint32_t kSeenAgeMs = 15000;
static constexpr uint32_t kTaskTickMs = 50;

struct Slot;

class SlotClientCbs : public NimBLEClientCallbacks {
 public:
  Slot *slot = nullptr;
  void onConnect(NimBLEClient *c) override;
  void onDisconnect(NimBLEClient *c) override;
  uint32_t onPassKeyRequest() override {
    Serial.printf("JK: passkey request → %u\n", (unsigned)JK_BMS_PIN);
    return (uint32_t)JK_BMS_PIN;
  }
  bool onConfirmPIN(uint32_t pin) override {
    Serial.printf("JK: confirm PIN %u\n", (unsigned)pin);
    return true;
  }
  void onAuthenticationComplete(ble_gap_conn_desc *desc) override {
    if (!desc) return;
    Serial.printf("JK: auth complete enc=%d bonded=%d\n", desc->sec_state.encrypted,
                  desc->sec_state.bonded);
  }
};

struct Slot {
  uint8_t idx = 0;
  NimBLEClient *client = nullptr;
  NimBLERemoteCharacteristic *chr_write = nullptr;
  NimBLERemoteCharacteristic *chr_notify = nullptr;
  NimBLEAddress target;
  bool have_target = false;

  // Manager-owned
  bool connected = false;
  bool want_connect = false;
  uint8_t resends = 0;
  uint32_t reconnect_ms = RECONNECT_MIN_MS;
  uint32_t next_try_at = 0;
  uint32_t last_init_ms = 0;
  uint32_t last_rssi_ms = 0;

  // Written from the NimBLE host task
  volatile bool disc_event = false;
  volatile uint32_t last_activity_ms = 0;
  volatile uint32_t last_decode_ms = 0;
  volatile uint32_t notify_count = 0;
  volatile uint32_t next_poll_ms = 0;
  JkFrameAssembler asmb;  // touched only by the notify callback while linked

  SlotClientCbs cbs;
};

static Slot g_slots[JK_BMS_COUNT];
static volatile uint32_t g_poll_ms = BMS_POLL_MS;  // BMS_POLL_SLEEP_MS while the screen sleeps

// Scan → manager hand-off (scan callback runs on the host task).
static volatile bool g_found = false;
static NimBLEAddress g_found_addr;
static char g_found_name[24];
static volatile int g_found_rssi = 0;
static bool g_scanning = false;

static inline bool elapsed(uint32_t now, uint32_t since, uint32_t ms) {
  return (int32_t)(now - since) >= (int32_t)ms;
}

static void copy_str(char *dst, size_t n, const char *src) {
  strncpy(dst, src ? src : "", n - 1);
  dst[n - 1] = '\0';
}

static void set_phase(uint8_t idx, const char *phase) {
  battery_state_lock();
  copy_str(battery_state(idx).ble_phase, sizeof(battery_state(idx).ble_phase), phase);
  battery_state_unlock();
}

static void update_seen(uint8_t idx, int rssi, const char *name, const NimBLEAddress &addr) {
  const std::string mac = addr.toString();
  battery_state_lock();
  BatteryState &s = battery_state(idx);
  s.ble_seen = true;
  if (rssi != 0) s.rssi_dbm = rssi;
  s.last_seen_ms = millis();
  if (name && name[0]) copy_str(s.ble_name, sizeof(s.ble_name), name);
  copy_str(s.ble_mac, sizeof(s.ble_mac), mac.c_str());
  battery_state_unlock();
}

static uint8_t jk_cmd_crc(const uint8_t *data, size_t len) {
  uint8_t c = 0;
  for (size_t i = 0; i < len; i++) c = (uint8_t)(c + data[i]);
  return c;
}

static bool send_cmd(Slot &s, uint8_t code) {
  if (!s.chr_write || !s.client || !s.client->isConnected()) return false;
  // AA 55 90 EB <cmd> <len=0> ... byte-sum checksum
  uint8_t out[20] = {0xAA, 0x55, 0x90, 0xEB, code, 0x00};
  out[19] = jk_cmd_crc(out, 19);
  const bool ok = s.chr_write->writeValue(out, sizeof(out), false);
  Serial.printf("JK%u: TX %02X %s\n", s.idx + 1, code, ok ? "ok" : "FAIL");
  return ok;
}

// 0x97 device info, then 0x96 → BMS starts streaming cell-info frames.
static void send_init(Slot &s) {
  vTaskDelay(pdMS_TO_TICKS(150));
  send_cmd(s, 0x97);
  vTaskDelay(pdMS_TO_TICKS(500));
  send_cmd(s, 0x96);
  s.last_init_ms = millis();
}

void SlotClientCbs::onConnect(NimBLEClient *) { Serial.printf("JK%u: connected\n", slot->idx + 1); }

void SlotClientCbs::onDisconnect(NimBLEClient *) {
  Serial.printf("JK%u: disconnected\n", slot->idx + 1);
  slot->disc_event = true;  // manager does the cleanup
}

static void on_notify(Slot *s, const uint8_t *data, size_t length) {
  const uint32_t now = millis();
  s->last_activity_ms = now;
  const uint32_t n = s->notify_count + 1;
  s->notify_count = n;
  if (n <= 3) Serial.printf("JK%u: notify %u B %02X %02X\n", s->idx + 1, (unsigned)length,
                            length > 0 ? data[0] : 0, length > 1 ? data[1] : 0);

  // Throttle: between polls drop everything (BMS keeps streaming ~1/s).
  const bool settings_hdr = length >= 5 && data[0] == 0x55 && data[1] == 0xAA && data[4] == 0x01;
  if (!s->asmb.assembling && !settings_hdr && (int32_t)(now - s->next_poll_ms) < 0) return;

  PackData d;
  if (!jk_frame_feed(s->asmb, data, length, d)) return;

  s->last_decode_ms = now;
  s->next_poll_ms = now + g_poll_ms - kPollSlackMs;

  battery_state_lock();
  BatteryState &st = battery_state(s->idx);
  st.d = d;
  st.has_data = true;
  st.last_update_ms = now;
  st.notify_count = n;
  st.ble_seen = true;
  st.last_seen_ms = now;
  copy_str(st.ble_phase, sizeof(st.ble_phase), "DATA");
  battery_state_unlock();

  Serial.printf("JK%u: SOC=%.0f%% V=%.2f I=%.2fA P=%.0fW dV=%.0fmV err=%08X\n", s->idx + 1,
                d.soc_pct, d.voltage_v, d.current_a, d.power_w, d.cell_delta_v * 1000.0f,
                (unsigned)d.errors_bitmask);
}

class ScanCbs : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice *adv) override {
    if (g_found) return;
    if (!adv->haveName() || adv->getName().rfind("JK_", 0) != 0) return;
    const NimBLEAddress addr = adv->getAddress();
    for (const Slot &s : g_slots) {
      if (s.have_target && s.target.equals(addr)) return;  // already ours
    }
    copy_str(g_found_name, sizeof(g_found_name), adv->getName().c_str());
    g_found_addr = addr;
    g_found_rssi = adv->getRSSI();
    g_found = true;
    NimBLEDevice::getScan()->stop();
  }
} g_scan_cbs;

static bool resolve_chars(Slot &s, NimBLERemoteService *svc) {
  s.chr_write = nullptr;
  s.chr_notify = nullptr;
  // JK often exposes TWO FFE1 chars (write + notify) — force rediscovery.
  std::vector<NimBLERemoteCharacteristic *> *chars = svc->getCharacteristics(true);
  if (!chars) return false;
  for (auto *c : *chars) {
    if (!c || !c->getUUID().equals(NimBLEUUID(kCharUuid))) continue;
    const bool can_w = c->canWrite() || c->canWriteNoResponse();
    const bool can_n = c->canNotify() || c->canIndicate();
    if (can_w && can_n) {
      s.chr_write = c;
      s.chr_notify = c;
      break;
    }
    if (can_w && !s.chr_write) s.chr_write = c;
    if (can_n && !s.chr_notify) s.chr_notify = c;
  }
  return s.chr_write && s.chr_notify;
}

static bool connect_slot(Slot &s) {
  if (!s.client) {
    s.client = NimBLEDevice::createClient();
    if (!s.client) {
      Serial.printf("JK%u: createClient failed\n", s.idx + 1);
      return false;
    }
    s.client->setClientCallbacks(&s.cbs, false);
    s.client->setConnectionParams(BLE_CONN_ITVL_MIN, BLE_CONN_ITVL_MAX, 0, BLE_CONN_TIMEOUT,
                                  BLE_SCAN_ITVL, BLE_SCAN_WINDOW);
    s.client->setConnectTimeout(BLE_CONNECT_TIMEOUT_S);
  }
  if (s.client->isConnected()) {
    s.client->disconnect();
    vTaskDelay(pdMS_TO_TICKS(300));
  }

  Serial.printf("JK%u: connecting %s\n", s.idx + 1, s.target.toString().c_str());
  set_phase(s.idx, "CONNECT");
  s.disc_event = false;
  if (!s.client->connect(s.target, true)) {
    // JK modules normally use a public MAC, but some advertise a random
    // (static) one — a wrong address type never connects. Alternate on failure.
    const uint8_t t = s.target.getType() == BLE_ADDR_PUBLIC ? BLE_ADDR_RANDOM : BLE_ADDR_PUBLIC;
    s.target = NimBLEAddress(s.target.toString(), t);
    Serial.printf("JK%u: connect failed (not in range / busy with the phone app?) — next try as %s\n",
                  s.idx + 1, t == BLE_ADDR_PUBLIC ? "public" : "random");
    return false;
  }

#if JK_BLE_SECURE
  s.client->secureConnection();
  for (int i = 0; i < 30 && s.client->isConnected(); i++) vTaskDelay(pdMS_TO_TICKS(100));
#endif
  vTaskDelay(pdMS_TO_TICKS(200));

  NimBLERemoteService *svc = s.client->getService(NimBLEUUID(kServiceUuid));
  if (!svc || !resolve_chars(s, svc)) {
    Serial.printf("JK%u: FFE0/FFE1 missing\n", s.idx + 1);
    s.client->disconnect();
    return false;
  }

  s.asmb.reset();
  s.next_poll_ms = millis();  // take the very first frame
  Slot *sp = &s;
  auto cb = [sp](NimBLERemoteCharacteristic *, uint8_t *p, size_t len, bool) { on_notify(sp, p, len); };
  const bool use_notify = s.chr_notify->canNotify();
  if (!s.chr_notify->subscribe(use_notify, cb, true)) {
    Serial.printf("JK%u: subscribe failed\n", s.idx + 1);
    s.client->disconnect();
    return false;
  }
  if (!s.client->isConnected()) return false;

  const int rssi = s.client->getRssi();
  update_seen(s.idx, rssi, nullptr, s.target);
  Serial.printf("JK%u: link RSSI=%d%s\n", s.idx + 1, rssi, rssi < -85 ? " (weak)" : "");
  s.client->updateConnParams(BLE_CONN_ITVL_MIN, BLE_CONN_ITVL_MAX, 0, BLE_CONN_TIMEOUT);

  const uint32_t now = millis();
  s.connected = true;
  s.resends = 0;
  s.reconnect_ms = RECONNECT_MIN_MS;
  s.last_activity_ms = now;
  s.last_decode_ms = 0;
  s.notify_count = 0;
  s.last_rssi_ms = now;
  battery_state_lock();
  battery_state(s.idx).connected = true;
  copy_str(battery_state(s.idx).ble_phase, sizeof(battery_state(s.idx).ble_phase), "LINK");
  battery_state_unlock();

  send_init(s);
  return true;
}

static void schedule_retry(Slot &s, uint32_t now) {
  s.next_try_at = now + std::max(s.reconnect_ms, (uint32_t)BLE_IDLE_GAP_MS);
  s.reconnect_ms = std::min(s.reconnect_ms * 2U, (uint32_t)RECONNECT_MAX_MS);
}

static void handle_disconnect(Slot &s, uint32_t now) {
  s.disc_event = false;
  if (!s.connected) return;  // failure during connect — already rescheduled
  s.connected = false;
  s.chr_write = nullptr;
  s.chr_notify = nullptr;
  battery_state_lock();
  battery_state(s.idx).connected = false;
  copy_str(battery_state(s.idx).ble_phase, sizeof(battery_state(s.idx).ble_phase), "FAIL");
  battery_state_unlock();
  // First retry soon, then back off.
  s.next_try_at = now + RECONNECT_MIN_MS;
  s.reconnect_ms = std::min(s.reconnect_ms * 2U, (uint32_t)RECONNECT_MAX_MS);
}

// Link watchdog: silence or undecodable stream → re-init, then drop the link.
static void watchdog(Slot &s, uint32_t now) {
  const uint32_t since_init = s.last_init_ms;
  const bool silent = elapsed(now, s.last_activity_ms, BLE_SILENCE_MS) &&
                      elapsed(now, since_init, BLE_SILENCE_MS);
  const uint32_t ref = s.last_decode_ms && (int32_t)(s.last_decode_ms - since_init) > 0
                           ? s.last_decode_ms
                           : since_init;
  const bool no_decode = elapsed(now, ref, BLE_DECODE_TIMEOUT_MS) &&
                         elapsed(now, s.next_poll_ms, BLE_DECODE_TIMEOUT_MS / 2);
  if (!silent && !no_decode) {
    if (s.last_decode_ms && (int32_t)(s.last_decode_ms - since_init) > 0) s.resends = 0;
    return;
  }
  if (s.resends >= BLE_MAX_RESENDS) {
    Serial.printf("JK%u: no data after %u re-inits — reconnect\n", s.idx + 1, s.resends);
    s.resends = 0;
    s.client->disconnect();  // onDisconnect → handle_disconnect
    s.last_init_ms = now;    // don't fire again while the disconnect settles
    return;
  }
  s.resends++;
  Serial.printf("JK%u: %s — re-init #%u\n", s.idx + 1, silent ? "silent" : "no frames", s.resends);
  set_phase(s.idx, silent ? "STALE" : "NODATA");
  s.next_poll_ms = now;
  send_init(s);
}

static bool any_connected() {
  for (const Slot &s : g_slots)
    if (s.connected) return true;
  return false;
}

static void manage(uint32_t now) {
  // 1) Disconnect events
  for (Slot &s : g_slots)
    if (s.disc_event) handle_disconnect(s, now);

  // 2) Scan results → assign to the first slot without a target
  if (g_found) {
    for (Slot &s : g_slots) {
      if (s.have_target) continue;
      bool dup = false;
      for (const Slot &o : g_slots) dup |= o.have_target && o.target.equals(g_found_addr);
      if (!dup) {
        s.target = g_found_addr;
        s.have_target = true;
        s.want_connect = true;
        update_seen(s.idx, g_found_rssi, g_found_name, g_found_addr);
        set_phase(s.idx, "SEEN");
        Serial.printf("JK%u: found %s (%s) RSSI=%d\n", s.idx + 1,
                      g_found_addr.toString().c_str(), g_found_name, g_found_rssi);
      }
      break;
    }
    g_found = false;
  }

  // 3) Scan housekeeping
  NimBLEScan *scan = NimBLEDevice::getScan();
  if (g_scanning && !scan->isScanning()) {
    g_scanning = false;
    scan->clearResults();
    for (Slot &s : g_slots) {
      if (s.have_target) continue;
      battery_state_lock();
      battery_state(s.idx).ble_scanning = false;
      copy_str(battery_state(s.idx).ble_phase, sizeof(battery_state(s.idx).ble_phase), "NOSIGNAL");
      battery_state_unlock();
      schedule_retry(s, now);
    }
  }

  // 4) Per-slot: watchdog, RSSI, seen-aging
  for (Slot &s : g_slots) {
    if (s.connected) {
      if (elapsed(now, s.last_rssi_ms, kRssiEveryMs)) {
        s.last_rssi_ms = now;
        const int rssi = s.client->getRssi();
        battery_state_lock();
        BatteryState &st = battery_state(s.idx);
        if (rssi != 0) st.rssi_dbm = rssi;
        st.ble_seen = true;
        st.last_seen_ms = now;
        st.notify_count = s.notify_count;
        battery_state_unlock();
      }
      watchdog(s, now);
      now = millis();  // send_init blocks ~650 ms
    } else {
      battery_state_lock();
      BatteryState &st = battery_state(s.idx);
      if (st.ble_seen && elapsed(now, st.last_seen_ms, kSeenAgeMs)) st.ble_seen = false;
      battery_state_unlock();
    }
  }

  // 5) At most one blocking connect / scan start per pass
  if (g_scanning) return;
  for (Slot &s : g_slots) {
    if (s.connected || !s.have_target) continue;
    if (!s.want_connect && !elapsed(now, s.next_try_at, 0)) continue;
    s.want_connect = false;
    if (!connect_slot(s)) {
      set_phase(s.idx, "FAIL");
      schedule_retry(s, millis());
    }
    return;
  }

  bool need_scan = false;
  for (const Slot &s : g_slots) need_scan |= !s.have_target && elapsed(now, s.next_try_at, 0);
  if (need_scan) {
    Serial.println("JK: scanning for JK_* ...");
    for (Slot &s : g_slots) {
      if (s.have_target) continue;
      battery_state_lock();
      battery_state(s.idx).ble_scanning = true;
      copy_str(battery_state(s.idx).ble_phase, sizeof(battery_state(s.idx).ble_phase), "SCAN");
      battery_state_unlock();
      s.next_try_at = now + BLE_IDLE_GAP_MS;  // re-armed by scan end anyway
    }
    scan->setAdvertisedDeviceCallbacks(&g_scan_cbs, false);
    scan->setActiveScan(false);
    scan->setInterval(BLE_SCAN_ITVL);
    // Leave airtime for the already-established link.
    scan->setWindow(any_connected() ? BLE_SCAN_ITVL / 2 : BLE_SCAN_WINDOW);
    g_found = false;
    g_scanning = scan->start(BLE_SCAN_SECONDS, nullptr, false);
  }
}

static void ble_task(void *) {
  for (;;) {
    manage(millis());
    mppt_manage(millis());
    vTaskDelay(pdMS_TO_TICKS(kTaskTickMs));
  }
}

void jk_bms_begin() {
  const char *macs[2] = {JK_BMS_MAC_1, JK_BMS_MAC_2};
  NimBLEDevice::init("cyd-jk-bms");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
#if JK_BLE_SECURE
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityPasskey((uint32_t)JK_BMS_PIN);
#else
  NimBLEDevice::setSecurityAuth(false, false, false);
#endif

  const uint32_t now = millis();
  for (uint8_t i = 0; i < JK_BMS_COUNT; i++) {
    Slot &s = g_slots[i];
    s.idx = i;
    s.cbs.slot = &s;
    // Stagger the first attempts so WiFi comes up first and links don't collide.
    s.next_try_at = now + 8000 + 4000UL * i;  // after the first WiFi join settles
    const char *mac = macs[i];
    if (mac && mac[0]) {
      s.target = NimBLEAddress(std::string(mac), BLE_ADDR_PUBLIC);
      s.have_target = true;
      Serial.printf("JK%u: MAC %s\n", i + 1, mac);
    } else {
      Serial.printf("JK%u: no MAC — will scan JK_*\n", i + 1);
    }
    set_phase(i, s.have_target ? "WAIT" : "SCAN");
  }

  mppt_begin(now);
  xTaskCreatePinnedToCore(ble_task, "jk_ble", 8192, nullptr, 1, nullptr, 1);
}

bool jk_bms_is_connected(uint8_t idx) {
  return idx < JK_BMS_COUNT && g_slots[idx].connected;
}

void jk_bms_set_sleep(bool sleep) {
  g_poll_ms = sleep ? BMS_POLL_SLEEP_MS : BMS_POLL_MS;
  mppt_set_sleep(sleep);
  if (!sleep) {
    const uint32_t now = millis();
    for (Slot &s : g_slots) s.next_poll_ms = now;  // fresh data right after wake
  }
}
