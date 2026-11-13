#pragma once

#include "battery_state.h"
#include <stddef.h>
#include <stdint.h>

// Per-connection reassembly buffer (one per BMS — frames arrive in ~20-244 B
// BLE chunks and two links must not mix).
struct JkFrameAssembler {
  static constexpr size_t kMax = 320;
  uint8_t buf[kMax];
  size_t len = 0;
  bool assembling = false;
  float design_ah = 0;  // from 0x01 settings frame, kept across frames
  void reset() {
    len = 0;
    assembling = false;
  }
};

// Feeds one notification chunk. Returns true when a complete, CRC-valid
// cell-info frame was decoded into `out` (other fields of `out` untouched on
// failure).
bool jk_frame_feed(JkFrameAssembler &asmb, const uint8_t *data, size_t length, PackData &out);
