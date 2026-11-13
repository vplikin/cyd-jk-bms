#include "pack_summary.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"
#include "i18n.h"
#include "jk_alarms.h"

DvGrade dv_grade(const PackData &d) {
  if (d.cell_count < 2) return DvGrade::NA;
  const float mv = d.cell_delta_v * 1000.0f;
  if (fabsf(d.current_a) > DV_REST_MAX_A) return DvGrade::NA;
  if (d.soc_pct < DV_GRADE_MIN_SOC) return DvGrade::NA;
  if (d.cell_max_v >= DV_ABSORB_CELL_V) return DvGrade::NA;  // top balance in progress
  if (mv <= DV_GREEN_MV) return DvGrade::GREEN;
  if (mv <= DV_YELLOW_MV) return DvGrade::YELLOW;
  return DvGrade::RED;
}

uint16_t pack_issues(const BatteryState &s, uint32_t now_ms) {
  if (!s.has_data) return 0;
  if (!s.is_fresh(now_ms)) return ISSUE_LOST;

  const PackData &d = s.d;
  uint16_t r = 0;
  if (jk_alarm_active(d.errors_bitmask)) r |= ISSUE_BMS_ALARM;

  const float mv = d.cell_delta_v * 1000.0f;
  if (dv_grade(d) == DvGrade::RED ||
      (d.cell_count >= 2 && mv >= DV_HARD_ALARM_MV && d.cell_min_v >= DV_PLATEAU_MIN_V &&
       d.cell_max_v < DV_ABSORB_CELL_V)) {
    r |= ISSUE_IMBALANCE;
  }

  const float cells[2] = {d.temp1_c, d.temp2_c};
  for (float t : cells) {
    if (!temp_valid(t)) continue;
    if (t >= TEMP_CELL_WARN_HIGH_C) r |= ISSUE_TEMP_HIGH;
    if (t <= TEMP_CELL_WARN_LOW_C && d.current_a > DISCHARGE_ETA_MIN_A) r |= ISSUE_TEMP_LOW;
  }
  if (temp_valid(d.temp_mos_c) && d.temp_mos_c >= TEMP_MOS_WARN_HIGH_C) r |= ISSUE_TEMP_HIGH;

  if (!d.discharging_mos) r |= ISSUE_DSG_OFF;
  return r;
}

void pack_issue_text(const BatteryState &s, uint16_t issues, char *buf, uint8_t n) {
  if (!buf || n == 0) return;
  buf[0] = '\0';
  if (issues & ISSUE_BMS_ALARM) {
    jk_alarm_list(s.d.errors_bitmask, buf, n);
  } else if (issues & ISSUE_LOST) {
    snprintf(buf, n, tr("no link", "нет связи"));
  } else if (issues & ISSUE_IMBALANCE) {
    snprintf(buf, n, tr("imbalance %.0f mV", "разбаланс %.0f мВ"), s.d.cell_delta_v * 1000.0f);
  } else if (issues & ISSUE_TEMP_HIGH) {
    snprintf(buf, n, tr("overheat", "перегрев"));
  } else if (issues & ISSUE_TEMP_LOW) {
    snprintf(buf, n, tr("too cold", "холод"));
  } else if (issues & ISSUE_DSG_OFF) {
    snprintf(buf, n, tr("discharge off", "разряд выкл"));
  }
}

SystemSummary summarize(const BatteryState *packs, uint8_t count, uint32_t now_ms) {
  SystemSummary r;
  float v_sum = 0;
  float soc_w = 0;
  for (uint8_t i = 0; i < count; i++) {
    const BatteryState &s = packs[i];
    if (!s.is_fresh(now_ms)) continue;
    const float nom = s.nominal_ah();
    r.fresh++;
    v_sum += s.d.voltage_v;
    r.current_a += s.d.current_a;
    r.power_w += s.d.power_w;
    r.remaining_ah += s.remaining_ah();
    r.energy_wh += s.remaining_ah() * s.d.voltage_v;
    r.nominal_ah += nom;
    soc_w += s.d.soc_pct * nom;
  }
  if (r.fresh == 0) return r;

  r.voltage_v = v_sum / r.fresh;
  r.soc_pct = r.nominal_ah > 0 ? soc_w / r.nominal_ah : 0;
  if (r.soc_pct > 100.0f) r.soc_pct = 100.0f;
  if (r.soc_pct < 0.0f) r.soc_pct = 0.0f;

  if (r.current_a > DISCHARGE_ETA_MIN_A) {
    r.mode = PackMode::CHARGE;
    const float to_full = r.nominal_ah - r.remaining_ah;
    if (to_full > 0) r.eta_h = to_full / r.current_a;
  } else if (r.current_a < -DISCHARGE_ETA_MIN_A) {
    r.mode = PackMode::DISCHARGE;
    if (r.remaining_ah > 0) r.eta_h = r.remaining_ah / -r.current_a;
  } else {
    r.mode = PackMode::IDLE;
  }
  if (r.eta_h > 999.0f) r.eta_h = -1;  // nonsense (tiny current)
  return r;
}
