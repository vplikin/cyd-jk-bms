#pragma once
// Copy to include/secrets.h (git-ignored) and fill in.
// Anything left out falls back to the default in config.h.

#define WIFI_SSID "my-wifi"
#define WIFI_PASS "my-password"
#define WIFI_HOSTNAME "cyd-jk-bms"

// Number of JK BMS packs in parallel (1 or 2).
#define JK_BMS_COUNT 1
// "" = auto-scan for the first JK_* device.
#define JK_BMS_MAC_1 ""
#define JK_BMS_MAC_2 ""
// EPEVER HN_xxxx gateway, e.g. "AA:BB:CC:DD:EE:FF". "" = no MPPT (page hidden).
#define MPPT_MAC ""

// POSIX time zone, e.g. "CET-1CEST,M3.5.0,M10.5.0/3", "EST5EDT,M3.2.0,M11.1.0".
#define TZ_INFO "UTC0"
// Weather location in decimal degrees, as strings. "" = no weather effects.
#define WEATHER_LAT ""
#define WEATHER_LON ""

// UI language: UI_LANG_EN or UI_LANG_RU.
#define UI_LANG UI_LANG_EN
