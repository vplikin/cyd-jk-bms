#pragma once

#include <stdint.h>

#include "battery_state.h"

enum class DvGrade : uint8_t { NA, GREEN, YELLOW, RED };

// Cell imbalance grade; NA when the reading is not meaningful (load, mid SOC,
// active top balance).
DvGrade dv_grade(const PackData &d);

// Problem bits for one pack.
enum : uint16_t {
  ISSUE_BMS_ALARM = 1u << 0,  // BMS protection / alarm bit
  ISSUE_LOST = 1u << 1,       // had data, now stale
  ISSUE_IMBALANCE = 1u << 2,
  ISSUE_TEMP_HIGH = 1u << 3,
  ISSUE_TEMP_LOW = 1u << 4,
  ISSUE_DSG_OFF = 1u << 5,  // discharge MOSFET off
};

uint16_t pack_issues(const BatteryState &s, uint32_t now_ms);
// Short Russian description of the most important issue ("" if none).
// Writes into buf (BMS alarm codes are listed).
void pack_issue_text(const BatteryState &s, uint16_t issues, char *buf, uint8_t n);

enum class PackMode : uint8_t { NONE, IDLE, CHARGE, DISCHARGE };

struct SystemSummary {
  uint8_t fresh = 0;  // packs with trusted data
  float soc_pct = 0;  // capacity-weighted
  float voltage_v = 0;
  float current_a = 0;
  float power_w = 0;
  float remaining_ah = 0;
  float nominal_ah = 0;
  float energy_wh = 0;  // sum of remaining_ah * voltage over fresh packs
  PackMode mode = PackMode::NONE;
  float eta_h = -1;  // to empty (discharge) or to full (charge); <0 = n/a
};

SystemSummary summarize(const BatteryState *packs, uint8_t count, uint32_t now_ms);

// Temperature probe looks valid (disconnected NTCs report garbage).
inline bool temp_valid(float c) { return c > -40.0f && c < 120.0f; }
