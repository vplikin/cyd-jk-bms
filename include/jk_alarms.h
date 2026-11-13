#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// JK app-style alarm codes per errors-bitmask bit (bit3/bit4 informational:
// e.g. "fully charged" — ignored).
inline const char *jk_alarm_code(uint8_t bit) {
  static const char *const kCodes[29] = {
      "WIRE",  "MOTP",  "CELL#", nullptr, nullptr, "OVP",   "COCP",  "CSCP",
      "COTP",  "CUTP",  "CPU",   "UVP",   "PUVP",  "DOCP",  "DSCP",  "DOTP",
      "CMOS",  "DMOS",  "GPS",   "PWD",   "DOFF",  "BOTP",  "TSNS",  "PL",
      "SCP",   "OCP2",  "OCP3",  "DUTP",  "LOCK",
  };
  return bit < 29 ? kCodes[bit] : nullptr;
}

// True if any bit that maps to a real alarm code is set.
inline bool jk_alarm_active(uint32_t mask) {
  for (uint8_t i = 0; i < 29; i++) {
    if ((mask & (1UL << i)) && jk_alarm_code(i)) return true;
  }
  return false;
}

// All active codes joined by spaces ("" if none). Truncates safely.
inline void jk_alarm_list(uint32_t mask, char *buf, size_t n) {
  if (!buf || n == 0) return;
  buf[0] = '\0';
  size_t len = 0;
  for (uint8_t i = 0; i < 29; i++) {
    const char *c = jk_alarm_code(i);
    if (!(mask & (1UL << i)) || !c) continue;
    const size_t cl = strlen(c);
    const size_t need = cl + (len ? 1 : 0);
    if (len + need + 1 > n) break;
    if (len) buf[len++] = ' ';
    memcpy(buf + len, c, cl);
    len += cl;
    buf[len] = '\0';
  }
}
