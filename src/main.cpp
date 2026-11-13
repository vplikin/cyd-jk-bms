#include <Arduino.h>

#include "battery_state.h"
#include "config.h"
#include "cyd_display.h"
#include "http_metrics.h"
#include "jk_bms_client.h"
#include "ui.h"

void setup() {
  setCpuFrequencyMhz(CPU_FREQ_MHZ);  // before Wi-Fi/BLE start
  battery_state_init();  // mutex before any other task can touch the state
  Serial.begin(115200);
  delay(200);
  Serial.printf("cyd-jk-bms: CYD + %u x JK BMS, CPU %u MHz\n", (unsigned)JK_BMS_COUNT,
                (unsigned)getCpuFrequencyMhz());

  cyd_display_init();
  ui_init();

  // Establish WiFi before starting NimBLE on the shared radio.
  http_metrics_begin();
  delay(100);
  jk_bms_begin();  // BLE runs in its own task
}

void loop() {
  cyd_display_loop();  // LVGL timers: touch, UI refresh, redraw
  delay(5);
}
