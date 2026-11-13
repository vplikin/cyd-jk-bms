#pragma once

#include <stdint.h>

#include "config.h"

// Telemetry decoded from one JK02 cell-info frame.
struct PackData {
  float voltage_v = 0;
  float current_a = 0;  // + charge, - discharge
  float power_w = 0;    // signed = V * I
  float soc_pct = 0;
  float soh_pct = 0;
  float capacity_remaining_ah = 0;
  float capacity_nominal_ah = 0;  // BMS-learned full-charge capacity (0x02 frame)
  float capacity_design_ah = 0;   // user-set battery capacity (0x01 settings frame)
  float temp_mos_c = 0;
  float temp1_c = 0;
  float temp2_c = 0;
  float cell_min_v = 0;
  float cell_max_v = 0;
  float cell_delta_v = 0;
  uint8_t cell_min_idx = 0;
  uint8_t cell_max_idx = 0;
  uint8_t cell_count = 0;
  uint32_t cycle_count = 0;
  uint32_t errors_bitmask = 0;  // JK02 protection/alarm bits (0 = ok)
  bool charging_mos = false;
  bool discharging_mos = false;
  bool balancing = false;
};

// One BMS slot: link diagnostics + last decoded telemetry.
struct BatteryState {
  bool connected = false;
  bool has_data = false;  // at least one frame decoded since boot
  uint32_t last_update_ms = 0;

  bool ble_seen = false;  // advertising or connected recently
  bool ble_scanning = false;
  int rssi_dbm = 0;  // last known RSSI (0 = unknown)
  uint32_t last_seen_ms = 0;
  uint32_t notify_count = 0;
  char ble_phase[12] = "BOOT";  // SCAN / SEEN / CONNECT / LINK / DATA / STALE / NODATA / FAIL / NOSIGNAL
  char ble_name[24] = "";
  char ble_mac[18] = "";

  PackData d;

  // Fresh = decoded recently enough to be trusted on screen.
  bool is_fresh(uint32_t now_ms) const {
    return has_data && (int32_t)(now_ms - last_update_ms) <= (int32_t)STALE_TIMEOUT_MS;
  }
  float nominal_ah() const {
    return d.capacity_nominal_ah > 0.1f ? d.capacity_nominal_ah : BATTERY_CAPACITY_AH;
  }
  float remaining_ah() const {
    return d.capacity_remaining_ah > 0.01f ? d.capacity_remaining_ah
                                           : (d.soc_pct / 100.0f) * nominal_ah();
  }
};

void battery_state_init();
void battery_state_lock();
void battery_state_unlock();
// Caller must hold the lock.
BatteryState &battery_state(uint8_t idx);
// Consistent copy of all slots (takes the lock itself).
void battery_state_snapshot(BatteryState out[JK_BMS_COUNT]);
