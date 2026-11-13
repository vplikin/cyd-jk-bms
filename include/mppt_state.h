#pragma once

#include <stdint.h>

#include "config.h"

// EPEVER MPPT (via the HN_xxxx BLE gateway, Modbus RTU input registers).
struct MpptData {
  float pv_v = 0, pv_a = 0, pv_w = 0;   // 0x3100..0x3103
  float bat_v = 0, bat_a = 0, bat_w = 0;  // 0x3104..0x3107 (bat_a = charging current)
  float temp_bat_c = 0, temp_dev_c = 0;   // 0x3110, 0x3111
  uint16_t bat_status = 0;                // 0x3200
  uint16_t chg_status = 0;                // 0x3201
  float gen_today_kwh = 0;                // 0x330C/0x330D
  float gen_total_kwh = 0;                // 0x3312/0x3313
  bool has_temps = false, has_status = false, has_energy = false;

  // 0 none, 1 float, 2 boost, 3 equalize
  uint8_t stage() const { return (chg_status >> 2) & 0x3; }
  bool fault() const { return (chg_status & 0x2) || (bat_status & 0x0F) == 4; }
};

struct MpptState {
  bool connected = false;
  bool has_data = false;
  uint32_t last_update_ms = 0;
  int rssi_dbm = 0;
  uint32_t ok_count = 0, err_count = 0;
  char phase[12] = "BOOT";  // WAIT / CONNECT / LINK / DATA / NOREPLY / FAIL
  MpptData d;

  bool is_fresh(uint32_t now_ms) const {
    return has_data && (int32_t)(now_ms - last_update_ms) <= (int32_t)MPPT_STALE_MS;
  }
};

// Uses the battery_state mutex (one lock for all shared telemetry).
MpptState &mppt_state();                // caller holds battery_state_lock()
MpptState mppt_state_snapshot();        // takes the lock itself
