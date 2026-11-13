#include <string.h>
#include <unity.h>

#include "jk_frame_decoder.h"

// Synthetic JK02 32S frames (field offsets as in ESPHome jk_bms_ble).
static uint8_t f[300];

static void put16(size_t i, uint16_t v) {
  f[i] = v & 0xFF;
  f[i + 1] = v >> 8;
}
static void put32(size_t i, uint32_t v) {
  put16(i, v & 0xFFFF);
  put16(i + 2, v >> 16);
}
static void seal() {
  uint8_t c = 0;
  for (int i = 0; i < 299; i++) c = (uint8_t)(c + f[i]);
  f[299] = c;
}
static void header(uint8_t type) {
  memset(f, 0, sizeof f);
  f[0] = 0x55;
  f[1] = 0xAA;
  f[2] = 0xEB;
  f[3] = 0x90;
  f[4] = type;
}

static void build_cell_frame() {
  header(0x02);
  for (int i = 0; i < 16; i++) put16(6 + i * 2, 3300 + i);  // 3.300..3.315 V
  put16(6 + 6 * 2, 3290);                                    // min: cell 7
  const int o = 32;
  put32(118 + o, 53000);                 // 53.000 V
  put32(126 + o, (uint32_t)-12500);      // -12.5 A
  put16(130 + o, 215);                   // 21.5 C
  put16(132 + o, 223);                   // 22.3 C
  put16(112 + o, 301);                   // MOS 30.1 C
  put32(134 + o, 1u << 5);               // OVP
  f[140 + o] = 1;                        // balancing
  f[141 + o] = 87;                       // SOC
  put32(142 + o, 243600);                // 243.6 Ah
  put32(146 + o, 280000);                // 280 Ah
  put32(150 + o, 42);                    // cycles
  f[158 + o] = 99;                       // SOH
  f[166 + o] = 1;                        // charge MOS
  f[167 + o] = 0;                        // discharge MOS
  seal();
}

void setUp() {}
void tearDown() {}

static void test_decodes_chunked_cell_frame() {
  build_cell_frame();
  JkFrameAssembler a;
  PackData d;
  // Arrives as BLE notifications of up to 128 bytes.
  TEST_ASSERT_FALSE(jk_frame_feed(a, f, 128, d));
  TEST_ASSERT_FALSE(jk_frame_feed(a, f + 128, 128, d));
  TEST_ASSERT_TRUE(jk_frame_feed(a, f + 256, 44, d));

  TEST_ASSERT_FLOAT_WITHIN(0.001f, 53.0f, d.voltage_v);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -12.5f, d.current_a);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, -662.5f, d.power_w);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 87.0f, d.soc_pct);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 99.0f, d.soh_pct);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 243.6f, d.capacity_remaining_ah);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 280.0f, d.capacity_nominal_ah);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 21.5f, d.temp1_c);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 22.3f, d.temp2_c);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.1f, d.temp_mos_c);
  TEST_ASSERT_EQUAL_UINT8(16, d.cell_count);
  TEST_ASSERT_EQUAL_UINT8(7, d.cell_min_idx);
  TEST_ASSERT_EQUAL_UINT8(16, d.cell_max_idx);
  TEST_ASSERT_FLOAT_WITHIN(0.0005f, 0.025f, d.cell_delta_v);
  TEST_ASSERT_EQUAL_UINT32(42, d.cycle_count);
  TEST_ASSERT_EQUAL_UINT32(1u << 5, d.errors_bitmask);
  TEST_ASSERT_TRUE(d.charging_mos);
  TEST_ASSERT_FALSE(d.discharging_mos);
  TEST_ASSERT_TRUE(d.balancing);
}

static void test_bad_crc_leaves_output_untouched() {
  build_cell_frame();
  f[299] ^= 0xFF;
  JkFrameAssembler a;
  PackData d;
  d.voltage_v = 1.0f;
  TEST_ASSERT_FALSE(jk_frame_feed(a, f, 300, d));
  TEST_ASSERT_EQUAL_FLOAT(1.0f, d.voltage_v);
  TEST_ASSERT_FALSE(a.assembling);
}

static void test_ignores_data_without_header() {
  build_cell_frame();
  JkFrameAssembler a;
  PackData d;
  TEST_ASSERT_FALSE(jk_frame_feed(a, f + 4, 200, d));
  TEST_ASSERT_EQUAL_UINT32(0, a.len);
  TEST_ASSERT_FALSE(jk_frame_feed(a, nullptr, 10, d));
  TEST_ASSERT_FALSE(jk_frame_feed(a, f, 0, d));
}

static void test_new_header_restarts_assembly() {
  build_cell_frame();
  JkFrameAssembler a;
  PackData d;
  TEST_ASSERT_FALSE(jk_frame_feed(a, f, 100, d));  // truncated frame
  TEST_ASSERT_FALSE(jk_frame_feed(a, f, 150, d));  // restart
  TEST_ASSERT_TRUE(jk_frame_feed(a, f + 150, 150, d));
}

static void test_settings_frame_sets_design_capacity() {
  header(0x01);
  put32(130, 280000);
  seal();
  JkFrameAssembler a;
  PackData d;
  TEST_ASSERT_FALSE(jk_frame_feed(a, f, 300, d));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 280.0f, a.design_ah);

  build_cell_frame();
  TEST_ASSERT_TRUE(jk_frame_feed(a, f, 300, d));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 280.0f, d.capacity_design_ah);
}

static void test_rejects_insane_values() {
  build_cell_frame();
  put32(118 + 32, 90000);  // 90 V: not a 16S LFP pack
  put32(118, 90000);       // and not as 24S either
  seal();
  JkFrameAssembler a;
  PackData d;
  TEST_ASSERT_FALSE(jk_frame_feed(a, f, 300, d));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_decodes_chunked_cell_frame);
  RUN_TEST(test_bad_crc_leaves_output_untouched);
  RUN_TEST(test_ignores_data_without_header);
  RUN_TEST(test_new_header_restarts_assembly);
  RUN_TEST(test_settings_frame_sets_design_capacity);
  RUN_TEST(test_rejects_insane_values);
  return UNITY_END();
}
