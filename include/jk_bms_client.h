#pragma once

#include <stdint.h>

// Starts NimBLE and the background "jk_ble" task that manages all BMS links.
void jk_bms_begin();
bool jk_bms_is_connected(uint8_t idx);
// Screen sleep: poll every BMS_POLL_SLEEP_MS; on wake poll immediately.
void jk_bms_set_sleep(bool sleep);
