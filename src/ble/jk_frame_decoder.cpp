#include "jk_frame_decoder.h"
#include "config.h"

#include <string.h>

#ifdef ARDUINO
#include <Arduino.h>
#define JK_LOG(...) Serial.printf(__VA_ARGS__)
#else
#include <stdio.h>
#define JK_LOG(...) printf(__VA_ARGS__)
#endif

static constexpr size_t kFrameMax = JkFrameAssembler::kMax;

static uint8_t jk_crc(const uint8_t *data, size_t len) {
  uint8_t c = 0;
  for (size_t i = 0; i < len; i++) c = (uint8_t)(c + data[i]);
  return c;
}

static uint16_t get16(const uint8_t *d, size_t i) {
  return (uint16_t)d[i] | ((uint16_t)d[i + 1] << 8);
}

static uint32_t get32(const uint8_t *d, size_t i) {
  return (uint32_t)get16(d, i) | ((uint32_t)get16(d, i + 2) << 16);
}

static int16_t get16s(const uint8_t *d, size_t i) {
  return (int16_t)get16(d, i);
}

static int32_t get32s(const uint8_t *d, size_t i) {
  return (int32_t)get32(d, i);
}

static bool decode_cell_info_with_offset(const uint8_t *data, bool use_32s, PackData &out) {
  // ESPHome: JK02_32S offset=16 for cells, then offset*=2 for telemetry
  uint8_t offset = use_32s ? 16 : 0;
  uint8_t cells = (uint8_t)(24 + (offset / 2));

  float min_v = 100.0f;
  float max_v = -100.0f;
  uint8_t min_i = 0;
  uint8_t max_i = 0;
  uint8_t enabled = 0;

  for (uint8_t i = 0; i < cells; i++) {
    float v = get16(data, i * 2 + 6) * 0.001f;
    if (v > 0.5f && v < 5.0f) {
      enabled++;
      if (v < min_v) {
        min_v = v;
        min_i = (uint8_t)(i + 1);
      }
      if (v > max_v) {
        max_v = v;
        max_i = (uint8_t)(i + 1);
      }
    }
  }

  offset = (uint8_t)(offset * 2); // 0 or 32

  float voltage = get32(data, 118 + offset) * 0.001f;
  float current = get32s(data, 126 + offset) * 0.001f;
  float soc = data[141 + offset];

  // Sanity: LiFePO4 pack roughly 10–60V, SOC 0–100
  if (voltage < 5.0f || voltage > 70.0f) return false;
  if (soc > 100.0f) return false;
  if (enabled == 0) return false;

  // MOS temperature: 32S @ 112+offset, 24S @ 134 (ESPHome jk_bms_ble).
  float temp_mos = get16s(data, use_32s ? 112 + offset : 134) * 0.1f;
  float temp1 = get16s(data, 130 + offset) * 0.1f;
  float temp2 = get16s(data, 132 + offset) * 0.1f;
  float rem_ah = get32(data, 142 + offset) * 0.001f;
  float nom_ah = get32(data, 146 + offset) * 0.001f;
  uint32_t cycles = get32(data, 150 + offset);
  float soh = data[158 + offset];
  // ESPHome: 32S errors @ 134+offset (32-bit LE); 24S @ 136 (16-bit LE)
  uint32_t errors = use_32s ? get32(data, 134 + offset) : get16(data, 136 + offset);
  bool charge_mos = data[166 + offset] != 0;
  bool discharge_mos = data[167 + offset] != 0;
  bool balancing = data[140 + offset] != 0;

  out.voltage_v = voltage;
  out.current_a = current;
  out.power_w = voltage * current;
  out.soc_pct = soc;
  out.soh_pct = soh <= 100.0f ? soh : 0;
  out.capacity_remaining_ah = rem_ah;
  out.capacity_nominal_ah = nom_ah;
  out.temp_mos_c = temp_mos;
  out.temp1_c = temp1;
  out.temp2_c = temp2;
  out.cell_count = enabled;
  out.cell_min_v = enabled ? min_v : 0;
  out.cell_max_v = enabled ? max_v : 0;
  out.cell_delta_v = enabled ? (max_v - min_v) : 0;
  out.cell_min_idx = min_i;
  out.cell_max_idx = max_i;
  out.cycle_count = cycles;
  out.errors_bitmask = errors;
  out.charging_mos = charge_mos;
  out.discharging_mos = discharge_mos;
  out.balancing = balancing;
  return true;
}

static bool decode_cell_info(const uint8_t *data, size_t size, PackData &out) {
  if (size < 300) return false;
  if (data[4] != 0x02) return false;
  if (data[299] != jk_crc(data, 299)) {
    JK_LOG("JK: cell frame CRC bad (got %02X want %02X)\n", data[299], jk_crc(data, 299));
    return false;
  }

  // Prefer configured layout, then fall back
  if (JK_PROTOCOL_32S) {
    if (decode_cell_info_with_offset(data, true, out)) return true;
    if (decode_cell_info_with_offset(data, false, out)) {
      JK_LOG("JK: decoded as 24S (fallback)\n");
      return true;
    }
  } else {
    if (decode_cell_info_with_offset(data, false, out)) return true;
    if (decode_cell_info_with_offset(data, true, out)) {
      JK_LOG("JK: decoded as 32S (fallback)\n");
      return true;
    }
  }
  JK_LOG("JK: cell frame CRC ok but values insane\n");
  return false;
}

bool jk_frame_feed(JkFrameAssembler &a, const uint8_t *data, size_t length, PackData &out) {
  if (!data || length == 0) return false;

  if (length >= 4 && data[0] == 0x55 && data[1] == 0xAA && data[2] == 0xEB && data[3] == 0x90) {
    a.len = 0;
    a.assembling = true;
  }

  if (!a.assembling) return false;

  if (a.len + length > kFrameMax) {
    a.reset();
    return false;
  }

  memcpy(a.buf + a.len, data, length);
  a.len += length;

  if (a.len < 300) return false;

  const uint8_t type = a.buf[4];
  bool ok = false;
  if (type == 0x02) {
    ok = decode_cell_info(a.buf, a.len, out);
    if (ok) out.capacity_design_ah = a.design_ah;
  } else if (type == 0x01 && a.len >= 300 && a.buf[299] == jk_crc(a.buf, 299)) {
    // ESPHome jk02 settings: "Nominal battery capacity" u32 @130, 0.001 Ah (same for 24S/32S)
    float cap = get32(a.buf, 130) * 0.001f;
    if (cap > 1.0f && cap < 2000.0f) {
      a.design_ah = cap;
      JK_LOG("JK: settings capacity %.1f Ah\n", cap);
    }
  }
  // 0x03 device info — not needed, dropped.
  a.reset();
  return ok;
}
