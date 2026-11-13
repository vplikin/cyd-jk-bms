#include <unity.h>

#include "jk_alarms.h"

void setUp() {}
void tearDown() {}

static void test_codes_map_bits() {
  TEST_ASSERT_EQUAL_STRING("WIRE", jk_alarm_code(0));
  TEST_ASSERT_EQUAL_STRING("OVP", jk_alarm_code(5));
  TEST_ASSERT_EQUAL_STRING("LOCK", jk_alarm_code(28));
  TEST_ASSERT_NULL(jk_alarm_code(3));  // informational
  TEST_ASSERT_NULL(jk_alarm_code(4));
  TEST_ASSERT_NULL(jk_alarm_code(29));
  TEST_ASSERT_NULL(jk_alarm_code(255));
}

static void test_active_ignores_informational_bits() {
  TEST_ASSERT_FALSE(jk_alarm_active(0));
  TEST_ASSERT_FALSE(jk_alarm_active((1u << 3) | (1u << 4)));
  TEST_ASSERT_FALSE(jk_alarm_active(1u << 30));
  TEST_ASSERT_TRUE(jk_alarm_active(1u << 11));
}

static void test_list_joins_codes() {
  char buf[32];
  jk_alarm_list((1u << 5) | (1u << 11) | (1u << 3), buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("OVP UVP", buf);
  jk_alarm_list(0, buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_list_truncates_on_whole_codes() {
  char buf[8];
  jk_alarm_list((1u << 5) | (1u << 11) | (1u << 13), buf, sizeof buf);
  TEST_ASSERT_EQUAL_STRING("OVP UVP", buf);  // "DOCP" does not fit
  char tiny[3] = {'x', 'x', 'x'};
  jk_alarm_list(1u << 5, tiny, sizeof tiny);
  TEST_ASSERT_EQUAL_STRING("", tiny);
  jk_alarm_list(1u << 5, nullptr, 10);  // must not crash
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_codes_map_bits);
  RUN_TEST(test_active_ignores_informational_bits);
  RUN_TEST(test_list_joins_codes);
  RUN_TEST(test_list_truncates_on_whole_codes);
  return UNITY_END();
}
