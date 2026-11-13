#include <unity.h>

#include "pack_summary.h"

void setUp() {}
void tearDown() {}

static constexpr uint32_t kNow = 100000;

// Healthy 16S pack at rest near full.
static BatteryState make_pack(float soc = 95.0f, float current = 0.0f, float nominal = 100.0f) {
  BatteryState s;
  s.has_data = true;
  s.last_update_ms = kNow - 1000;
  PackData &d = s.d;
  d.voltage_v = 53.2f;
  d.current_a = current;
  d.power_w = d.voltage_v * current;
  d.soc_pct = soc;
  d.capacity_nominal_ah = nominal;
  d.capacity_remaining_ah = nominal * soc / 100.0f;
  d.cell_count = 16;
  d.cell_min_v = 3.320f;
  d.cell_max_v = 3.330f;
  d.cell_delta_v = 0.010f;
  d.temp1_c = 22.0f;
  d.temp2_c = 23.0f;
  d.temp_mos_c = 25.0f;
  d.discharging_mos = true;
  d.charging_mos = true;
  return s;
}

static void test_dv_grade_thresholds() {
  BatteryState s = make_pack();
  TEST_ASSERT_EQUAL(DvGrade::GREEN, dv_grade(s.d));
  s.d.cell_delta_v = 0.035f;
  TEST_ASSERT_EQUAL(DvGrade::YELLOW, dv_grade(s.d));
  s.d.cell_delta_v = 0.080f;
  TEST_ASSERT_EQUAL(DvGrade::RED, dv_grade(s.d));
}

static void test_dv_grade_not_applicable() {
  BatteryState s = make_pack();
  s.d.cell_delta_v = 0.080f;
  s.d.current_a = -5.0f;  // under load
  TEST_ASSERT_EQUAL(DvGrade::NA, dv_grade(s.d));
  s = make_pack(50.0f);  // mid plateau
  TEST_ASSERT_EQUAL(DvGrade::NA, dv_grade(s.d));
  s = make_pack();
  s.d.cell_max_v = 3.50f;  // top balance
  TEST_ASSERT_EQUAL(DvGrade::NA, dv_grade(s.d));
  s.d.cell_count = 1;
  TEST_ASSERT_EQUAL(DvGrade::NA, dv_grade(s.d));
}

static void test_issues_healthy_and_lost() {
  BatteryState s = make_pack();
  TEST_ASSERT_EQUAL_UINT16(0, pack_issues(s, kNow));
  TEST_ASSERT_EQUAL_UINT16(ISSUE_LOST, pack_issues(s, kNow + STALE_TIMEOUT_MS + 5000));
  BatteryState never;
  TEST_ASSERT_EQUAL_UINT16(0, pack_issues(never, kNow));
}

static void test_issues_flags() {
  BatteryState s = make_pack();
  s.d.errors_bitmask = 1u << 5;
  s.d.temp1_c = 55.0f;
  s.d.discharging_mos = false;
  const uint16_t i = pack_issues(s, kNow);
  TEST_ASSERT_TRUE(i & ISSUE_BMS_ALARM);
  TEST_ASSERT_TRUE(i & ISSUE_TEMP_HIGH);
  TEST_ASSERT_TRUE(i & ISSUE_DSG_OFF);
  TEST_ASSERT_FALSE(i & ISSUE_IMBALANCE);

  // Disconnected NTC reads garbage: ignored.
  s = make_pack();
  s.d.temp2_c = -60.0f;
  TEST_ASSERT_EQUAL_UINT16(0, pack_issues(s, kNow));

  // Cold only matters while charging.
  s.d.temp1_c = -2.0f;
  TEST_ASSERT_EQUAL_UINT16(0, pack_issues(s, kNow));
  s.d.current_a = 5.0f;
  TEST_ASSERT_TRUE(pack_issues(s, kNow) & ISSUE_TEMP_LOW);
}

static void test_hard_imbalance_under_load() {
  BatteryState s = make_pack(60.0f, -20.0f);
  s.d.cell_min_v = 3.15f;
  s.d.cell_max_v = 3.31f;
  s.d.cell_delta_v = 0.160f;
  TEST_ASSERT_TRUE(pack_issues(s, kNow) & ISSUE_IMBALANCE);
}

static void test_issue_text_priority() {
  BatteryState s = make_pack();
  s.d.errors_bitmask = (1u << 5) | (1u << 11);
  char buf[32];
  pack_issue_text(s, ISSUE_BMS_ALARM | ISSUE_DSG_OFF, buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("OVP UVP", buf);
  pack_issue_text(s, ISSUE_LOST | ISSUE_DSG_OFF, buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("no link", buf);
  s.d.cell_delta_v = 0.084f;
  pack_issue_text(s, ISSUE_IMBALANCE, buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("imbalance 84 mV", buf);
  pack_issue_text(s, 0, buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_summary_weights_by_capacity() {
  BatteryState p[2] = {make_pack(100.0f, -10.0f, 100.0f), make_pack(40.0f, -10.0f, 300.0f)};
  const SystemSummary r = summarize(p, 2, kNow);
  TEST_ASSERT_EQUAL_UINT8(2, r.fresh);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 55.0f, r.soc_pct);  // (100*100 + 40*300) / 400
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 400.0f, r.nominal_ah);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 220.0f, r.remaining_ah);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -20.0f, r.current_a);
  TEST_ASSERT_EQUAL(PackMode::DISCHARGE, r.mode);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 11.0f, r.eta_h);
}

static void test_summary_charge_idle_and_stale() {
  BatteryState p[2] = {make_pack(50.0f, 25.0f), make_pack(50.0f, 25.0f)};
  p[1].last_update_ms = 0;  // stale: excluded
  SystemSummary r = summarize(p, 2, kNow);
  TEST_ASSERT_EQUAL_UINT8(1, r.fresh);
  TEST_ASSERT_EQUAL(PackMode::CHARGE, r.mode);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f, r.eta_h);  // 50 Ah / 25 A

  p[0].d.current_a = 0.05f;
  r = summarize(p, 2, kNow);
  TEST_ASSERT_EQUAL(PackMode::IDLE, r.mode);
  TEST_ASSERT_TRUE(r.eta_h < 0);

  r = summarize(p, 0, kNow);
  TEST_ASSERT_EQUAL_UINT8(0, r.fresh);
  TEST_ASSERT_EQUAL(PackMode::NONE, r.mode);
}

static void test_nominal_falls_back_to_config() {
  BatteryState s = make_pack();
  s.d.capacity_nominal_ah = 0;
  s.d.capacity_remaining_ah = 0;
  s.d.soc_pct = 50.0f;
  TEST_ASSERT_FLOAT_WITHIN(0.01f, BATTERY_CAPACITY_AH, s.nominal_ah());
  TEST_ASSERT_FLOAT_WITHIN(0.01f, BATTERY_CAPACITY_AH / 2, s.remaining_ah());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_dv_grade_thresholds);
  RUN_TEST(test_dv_grade_not_applicable);
  RUN_TEST(test_issues_healthy_and_lost);
  RUN_TEST(test_issues_flags);
  RUN_TEST(test_hard_imbalance_under_load);
  RUN_TEST(test_issue_text_priority);
  RUN_TEST(test_summary_weights_by_capacity);
  RUN_TEST(test_summary_charge_idle_and_stale);
  RUN_TEST(test_nominal_falls_back_to_config);
  return UNITY_END();
}
