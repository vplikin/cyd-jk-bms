#include "battery_state.h"

#include "mppt_state.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static BatteryState g_packs[JK_BMS_COUNT];
static MpptState g_mppt;  // guarded by the same mutex
static SemaphoreHandle_t g_mutex = nullptr;

void battery_state_init() {
  if (!g_mutex) g_mutex = xSemaphoreCreateMutex();
}

BatteryState &battery_state(uint8_t idx) {
  return g_packs[idx < JK_BMS_COUNT ? idx : 0];
}

void battery_state_lock() {
  // battery_state_init() runs first in setup(), before any other task exists.
  if (!g_mutex) battery_state_init();
  xSemaphoreTake(g_mutex, portMAX_DELAY);
}

void battery_state_unlock() {
  if (g_mutex) xSemaphoreGive(g_mutex);
}

void battery_state_snapshot(BatteryState out[JK_BMS_COUNT]) {
  battery_state_lock();
  for (uint8_t i = 0; i < JK_BMS_COUNT; i++) out[i] = g_packs[i];
  battery_state_unlock();
}

MpptState &mppt_state() { return g_mppt; }

MpptState mppt_state_snapshot() {
  battery_state_lock();
  MpptState m = g_mppt;
  battery_state_unlock();
  return m;
}
