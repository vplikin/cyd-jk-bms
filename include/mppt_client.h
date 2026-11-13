#pragma once
#include <stdint.h>

// EPEVER MPPT over BLE. Runs inside the "jk_ble" task (single owner of NimBLE
// blocking calls): one persistent link, slow Modbus polling, no flash writes.
void mppt_begin(uint32_t now_ms);
void mppt_manage(uint32_t now_ms);
void mppt_set_sleep(bool sleep);
