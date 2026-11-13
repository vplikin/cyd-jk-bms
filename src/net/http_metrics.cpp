#include "http_metrics.h"

#include "battery_state.h"
#include "mppt_state.h"
#include "config.h"
#include "i18n.h"
#include "jk_alarms.h"
#include "pack_summary.h"
#include "weather.h"

#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_coexist.h>
#include <esp_wifi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static WebServer server(HTTP_METRICS_PORT);
static WiFiUDP udp;
static bool g_wifi_ok = false;
static uint32_t g_last_wifi_try_ms = 0;
static uint32_t g_last_push_ms = 0;
static char g_ip[16] = "";
static IPAddress g_bcast;

static void add_cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
}

static const char *mode_text(PackMode m) {
  switch (m) {
    case PackMode::CHARGE: return "charge";
    case PackMode::DISCHARGE: return "discharge";
    case PackMode::IDLE: return "idle";
    default: return "-";
  }
}

static const char *jb(bool b) { return b ? "true" : "false"; }

// Single JSON buffer: the HTTP handler and the UDP push both run in http_task.
// Two packs + MPPT come to ~2 KB; the dashboard reads datagrams up to 8 KB.
static char g_json[3072];

// Top-level keys keep the single-pack schema (dashboard compatibility) and
// describe the whole bank; "packs" carries per-BMS detail.
static size_t build_metrics_json(char *buf, size_t cap) {
  BatteryState packs[JK_BMS_COUNT];
  battery_state_snapshot(packs);
  const uint32_t now = millis();
  const SystemSummary sum = summarize(packs, JK_BMS_COUNT, now);

  // Bank-level extremes over fresh packs.
  float soh = 0, t1 = 0, t2 = 0, tmos = 0, dv = 0, cmin = 0, cmax = 0;
  uint32_t cycles = 0;
  int rssi = 0;
  unsigned cells = 0;
  bool any_conn = false, bal = false, first = true;
  char alarm[96] = "";
  for (uint8_t i = 0; i < JK_BMS_COUNT; i++) {
    const BatteryState &s = packs[i];
    any_conn |= s.connected;
    if (s.connected && s.rssi_dbm != 0 && (rssi == 0 || s.rssi_dbm < rssi)) rssi = s.rssi_dbm;
    const uint16_t is = pack_issues(s, now);
    if (is) {
      char t[40];
      pack_issue_text(s, is, t, sizeof(t));
      const size_t l = strlen(alarm);
      snprintf(alarm + l, sizeof(alarm) - l, "%s%u:%s", l ? " " : "", i + 1, t);
    }
    if (!s.is_fresh(now)) continue;
    const PackData &d = s.d;
    if (first) {
      soh = d.soh_pct;
      t1 = d.temp1_c;
      t2 = d.temp2_c;
      tmos = d.temp_mos_c;
      cmin = d.cell_min_v;
      cmax = d.cell_max_v;
      first = false;
    } else {
      soh = fminf(soh, d.soh_pct);
      t1 = fmaxf(t1, d.temp1_c);
      t2 = fmaxf(t2, d.temp2_c);
      tmos = fmaxf(tmos, d.temp_mos_c);
      cmin = fminf(cmin, d.cell_min_v);
      cmax = fmaxf(cmax, d.cell_max_v);
    }
    dv = fmaxf(dv, d.cell_delta_v * 1000.0f);
    cycles = cycles > d.cycle_count ? cycles : d.cycle_count;
    cells += d.cell_count;
    bal |= d.balancing;
  }
  if (!alarm[0]) snprintf(alarm, sizeof(alarm), tr("OK", "Норма"));
  const bool has = sum.fresh > 0;
  const float eta = sum.mode == PackMode::DISCHARGE ? sum.eta_h : -1.0f;
  const float eta_full = sum.mode == PackMode::CHARGE ? sum.eta_h : -1.0f;

  int n = snprintf(
      buf, cap,
      "{\"ts\":%lu,\"ip\":\"%s\",\"online\":%s,\"connected\":%s,\"stale\":%s,"
      "\"has_data\":%s,\"packs_total\":%u,\"packs_online\":%u,"
      "\"soc\":%.1f,\"soh\":%.1f,\"v\":%.3f,\"a\":%.3f,\"w\":%.1f,"
      "\"cap_rem\":%.3f,\"cap_nom\":%.3f,\"energy_wh\":%.1f,\"cycles\":%lu,"
      "\"t1\":%.1f,\"t2\":%.1f,\"t_mos\":%.1f,"
      "\"cell_min\":%.3f,\"cell_max\":%.3f,\"cell_dv_mv\":%.1f,\"cell_count\":%u,"
      "\"alarm\":\"%s\",\"balancing\":%s,\"rssi\":%d,"
      "\"eta_h\":%.2f,\"eta_full_h\":%.2f,\"mode\":\"%s\",\"packs\":[",
      (unsigned long)now, g_ip, jb(has), jb(any_conn), jb(any_conn && !has), jb(has),
      (unsigned)JK_BMS_COUNT, (unsigned)sum.fresh, sum.soc_pct, soh, sum.voltage_v,
      sum.current_a, sum.power_w, sum.remaining_ah, sum.nominal_ah, sum.energy_wh, (unsigned long)cycles, t1,
      t2, tmos, cmin, cmax, dv, cells, alarm, jb(bal), rssi, eta, eta_full,
      mode_text(sum.mode));
  if (n < 0 || (size_t)n >= cap) return 0;

  for (uint8_t i = 0; i < JK_BMS_COUNT; i++) {
    const BatteryState &s = packs[i];
    const PackData &d = s.d;
    const int m = snprintf(
        buf + n, cap - n,
        "%s{\"id\":%u,\"mac\":\"%s\",\"connected\":%s,\"online\":%s,\"age_s\":%ld,"
        "\"rssi\":%d,\"phase\":\"%s\",\"soc\":%.1f,\"soh\":%.1f,\"v\":%.3f,\"a\":%.3f,"
        "\"w\":%.1f,\"cap_rem\":%.3f,\"cap_nom\":%.3f,\"energy_wh\":%.1f,\"cycles\":%lu,\"t1\":%.1f,"
        "\"t2\":%.1f,\"t_mos\":%.1f,\"cell_min\":%.3f,\"cell_max\":%.3f,"
        "\"cell_dv_mv\":%.1f,\"cell_count\":%u,\"errors\":%lu,\"chg\":%s,\"dsg\":%s,"
        "\"bal\":%s}",
        i ? "," : "", (unsigned)(i + 1), s.ble_mac, jb(s.connected), jb(s.is_fresh(now)),
        s.has_data ? (long)((now - s.last_update_ms) / 1000) : -1L, s.rssi_dbm, s.ble_phase,
        d.soc_pct, d.soh_pct, d.voltage_v, d.current_a, d.power_w, s.remaining_ah(),
        s.nominal_ah(), s.remaining_ah() * d.voltage_v, (unsigned long)d.cycle_count, d.temp1_c, d.temp2_c, d.temp_mos_c,
        d.cell_min_v, d.cell_max_v, d.cell_delta_v * 1000.0f, (unsigned)d.cell_count,
        (unsigned long)d.errors_bitmask, jb(d.charging_mos), jb(d.discharging_mos),
        jb(d.balancing));
    if (m < 0 || (size_t)m >= cap - n) return 0;
    n += m;
  }
  {
    const MpptState mp = mppt_state_snapshot();
    const MpptData &d = mp.d;
    const int m = snprintf(
        buf + n, cap - n,
        "],\"mppt\":{\"connected\":%s,\"online\":%s,\"age_s\":%ld,\"rssi\":%d,"
        "\"phase\":\"%s\",\"pv_v\":%.2f,\"pv_a\":%.2f,\"pv_w\":%.1f,\"bat_v\":%.2f,"
        "\"bat_a\":%.2f,\"bat_w\":%.1f,\"stage\":%u,\"fault\":%s,\"bat_st\":%u,"
        "\"chg_st\":%u,\"t_bat\":%.1f,\"t_dev\":%.1f,\"kwh_today\":%.2f,\"kwh_total\":%.2f}}",
        jb(mp.connected), jb(mp.is_fresh(now)),
        mp.has_data ? (long)((now - mp.last_update_ms) / 1000) : -1L, mp.rssi_dbm, mp.phase,
        d.pv_v, d.pv_a, d.pv_w, d.bat_v, d.bat_a, d.bat_w, (unsigned)d.stage(), jb(d.fault()),
        (unsigned)d.bat_status, (unsigned)d.chg_status, d.temp_bat_c, d.temp_dev_c,
        d.gen_today_kwh, d.gen_total_kwh);
    if (m < 0 || (size_t)m >= cap - n) return 0;
    n += m;
  }
  if ((size_t)n + 1 > cap) return 0;
  buf[n] = '\0';
  return (size_t)n;
}

static void handle_metrics() {
  const size_t len = build_metrics_json(g_json, sizeof(g_json));
  add_cors();
  if (len == 0) {
    server.send(500, "application/json", "{\"error\":\"overflow\"}");
    return;
  }
  server.send(200, "application/json", g_json);
}

// Broadcast so the collector needs no fixed CYD address and unicast quirks
// (AP isolation, stale ARP) can't hide the device.
static void push_metrics() {
  if (!g_wifi_ok) return;
  const size_t len = build_metrics_json(g_json, sizeof(g_json));
  if (len == 0) return;
  if (udp.beginPacket(g_bcast, METRICS_UDP_PORT)) {
    udp.write((const uint8_t *)g_json, len);
    udp.endPacket();
  }
}

static void handle_options() {
  add_cors();
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  server.send(204);
}

static void on_wifi_got_ip() {
  snprintf(g_ip, sizeof(g_ip), "%s", WiFi.localIP().toString().c_str());
  g_wifi_ok = true;
  configTzTime(TZ_INFO, NTP_SERVER);  // SNTP keeps resyncing in the background
  // Derive the directed broadcast from the mask instead of forcing .255 — a
  // non-/24 lease would otherwise send the metrics push off-subnet.
  g_bcast = IPAddress((uint32_t)WiFi.localIP() | ~(uint32_t)WiFi.subnetMask());
  Serial.printf("WiFi OK %s  RSSI %d  metrics http://%s/\n", WIFI_SSID, WiFi.RSSI(),
                g_ip);
  server.stop();
  server.begin();
  Serial.printf("HTTP listening on :%d\n", (int)HTTP_METRICS_PORT);
  static bool mdns_ok = false;
  if (!mdns_ok && MDNS.begin(WIFI_HOSTNAME)) {
    MDNS.addService("http", "tcp", HTTP_METRICS_PORT);
    mdns_ok = true;
    Serial.printf("mDNS http://%s.local/\n", WIFI_HOSTNAME);
  }
}

static bool g_wifi_backoff_reset = false;

static void try_wifi() {
  if (!WIFI_SSID[0]) return;  // no network configured
  const wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    g_wifi_backoff_reset = true;
    if (!g_wifi_ok || g_ip[0] == '\0') on_wifi_got_ip();
    return;
  }

  if (g_wifi_ok) {
    Serial.printf("WiFi lost (status=%d) — reconnect\n", (int)st);
  }
  g_wifi_ok = false;
  g_ip[0] = '\0';

  // WiFi and BLE share one radio. Every WiFi.begin() sweeps all channels for
  // seconds and starves BLE connects, so back off: 15 s → 30 → 60 → 120 s.
  // (A 5 s retry also restarted connects that simply had not finished yet.)
  static uint32_t retry_ms = 15000;
  if (g_wifi_backoff_reset) {
    g_wifi_backoff_reset = false;
    retry_ms = 15000;
  }
  const uint32_t now = millis();
  if (now - g_last_wifi_try_ms < retry_ms) return;
  g_last_wifi_try_ms = now;
  retry_ms = retry_ms * 2 > 120000 ? 120000 : retry_ms * 2;
  Serial.printf("WiFi connecting to %s (next retry %lus)...\n", WIFI_SSID,
                (unsigned long)(retry_ms / 1000));
  WiFi.disconnect(false, false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static void http_task(void *) {
  for (;;) {
    try_wifi();
    for (int i = 0; i < 8; i++) server.handleClient();

    const uint32_t now = millis();
    if (now - g_last_push_ms >= METRICS_PUSH_MS) {
      g_last_push_ms = now;
      push_metrics();
    }
    weather_loop(g_wifi_ok && WiFi.status() == WL_CONNECTED);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

static bool wait_wifi(uint32_t timeout_ms) {
  if (!WIFI_SSID[0]) {
    Serial.println("WiFi: WIFI_SSID not set (include/secrets.h)");
    return false;
  }
  const uint32_t t0 = millis();
  Serial.printf("WiFi waiting for %s (max %lu ms)...\n", WIFI_SSID,
                (unsigned long)timeout_ms);
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < timeout_ms) {
    delay(200);
  }
  if (WiFi.status() == WL_CONNECTED) {
    on_wifi_got_ip();
    return true;
  }
  Serial.println("WiFi: not connected yet — will keep retrying");
  return false;
}

void http_metrics_begin() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(WIFI_HOSTNAME);
  // Modem sleep is required for WiFi/BLE coexistence.
  WiFi.setSleep(WIFI_PS_MIN_MODEM);
  esp_wifi_set_ps(WiFi.getSleep());
  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
  WiFi.setTxPower(WIFI_TX_POWER);
  if (WIFI_SSID[0]) WiFi.begin(WIFI_SSID, WIFI_PASS);
  g_last_wifi_try_ms = millis();

  server.on("/", HTTP_GET, handle_metrics);
  server.on("/", HTTP_OPTIONS, handle_options);
  server.on("/api/state", HTTP_GET, handle_metrics);
  server.on("/api/state", HTTP_OPTIONS, handle_options);
  server.onNotFound([]() {
    add_cors();
    server.send(404, "application/json", "{\"error\":\"not found\"}");
  });

  // Establish WiFi before starting NimBLE on the shared radio.
  wait_wifi(WIFI_CONNECT_TIMEOUT_MS);

  udp.begin(METRICS_UDP_PORT);
  g_last_push_ms = millis();
  Serial.printf("Metrics: HTTP :%d + UDP broadcast :%d every %lu ms\n",
                (int)HTTP_METRICS_PORT, (int)METRICS_UDP_PORT,
                (unsigned long)METRICS_PUSH_MS);
  xTaskCreatePinnedToCore(http_task, "http_metrics", 10240, nullptr, 4, nullptr, 0);
}

bool http_metrics_wifi_ok() { return g_wifi_ok && WiFi.status() == WL_CONNECTED; }

void http_metrics_ip(char *buf, size_t n) {
  if (!buf || n == 0) return;
  if (http_metrics_wifi_ok() && g_ip[0]) {
    snprintf(buf, n, "%s", g_ip);
  } else {
    buf[0] = '\0';
  }
}
