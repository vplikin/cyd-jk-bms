#pragma once

// Every setting below is a default. Local values (Wi-Fi, MAC addresses, pack
// count, time zone, location) go into include/secrets.h, which is included
// first and overrides anything here — copy include/secrets.example.h.
#if defined(CYD_NO_SECRETS)
// Host unit tests: defaults only, independent of local secrets.
#elif __has_include("secrets.h")
#include "secrets.h"
#else
#warning "include/secrets.h not found: copy include/secrets.example.h"
#endif

// ---------------------------------------------------------------------------
// BMS
// ---------------------------------------------------------------------------

// Number of JK BMS packs wired in parallel (1 or 2).
#ifndef JK_BMS_COUNT
#define JK_BMS_COUNT 1
#endif

// BMS MAC addresses (JK BMS app → device info). Empty "" = auto-scan for the
// first BLE name starting with "JK_" that is not already used by another slot.
#ifndef JK_BMS_MAC_1
#define JK_BMS_MAC_1 ""
#endif
#ifndef JK_BMS_MAC_2
#define JK_BMS_MAC_2 ""
#endif

// Nominal pack capacity (per pack) when the BMS hasn't reported its own yet.
#ifndef BATTERY_CAPACITY_AH
#define BATTERY_CAPACITY_AH 100.0f
#endif

// JK02_32S = modern Smart BMS (default). Set 0 for older JK02_24S layout.
#ifndef JK_PROTOCOL_32S
#define JK_PROTOCOL_32S 1
#endif

// BLE pairing PIN shown in JK BMS app (usually 1234)
#ifndef JK_BMS_PIN
#define JK_BMS_PIN 1234
#endif

// 1 = force secure/bond (PIN). Many JK work without — try 0 if no notifies.
#ifndef JK_BLE_SECURE
#define JK_BLE_SECURE 0
#endif

// How often a telemetry frame is taken from each BMS. JK streams frames on
// its own (~1/s); everything between polls is dropped in the BLE callback.
#ifndef BMS_POLL_MS
#define BMS_POLL_MS 5000UL
#endif

// Data older than this is shown as stale (must be > BMS_POLL_MS).
#ifndef STALE_TIMEOUT_MS
#define STALE_TIMEOUT_MS 35000UL
#endif

// No notifications at all for this long → re-send init commands.
#ifndef BLE_SILENCE_MS
#define BLE_SILENCE_MS 15000UL
#endif
// Notifications arrive but no frame decodes for this long → re-send init.
#ifndef BLE_DECODE_TIMEOUT_MS
#define BLE_DECODE_TIMEOUT_MS 20000UL
#endif
// After this many fruitless re-sends the link is dropped and re-established.
#ifndef BLE_MAX_RESENDS
#define BLE_MAX_RESENDS 3
#endif

#ifndef RECONNECT_MIN_MS
#define RECONNECT_MIN_MS 10000UL
#endif
#ifndef RECONNECT_MAX_MS
#define RECONNECT_MAX_MS 60000UL
#endif

// Pause between BLE connect/scan attempts.
#ifndef BLE_IDLE_GAP_MS
#define BLE_IDLE_GAP_MS 10000UL
#endif

// BLE scan interval/window in 0.625 ms units.
#ifndef BLE_SCAN_ITVL
#define BLE_SCAN_ITVL 100
#endif
#ifndef BLE_SCAN_WINDOW
#define BLE_SCAN_WINDOW 99  // near-continuous scan
#endif
#ifndef BLE_SCAN_SECONDS
#define BLE_SCAN_SECONDS 5
#endif
// Relaxed connection interval: polling is slow, and it leaves airtime for
// the other links and Wi-Fi.
#ifndef BLE_CONN_ITVL_MIN
#define BLE_CONN_ITVL_MIN 48  // 1.25ms units → 60 ms
#endif
#ifndef BLE_CONN_ITVL_MAX
#define BLE_CONN_ITVL_MAX 96  // 1.25ms units → 120 ms
#endif
#ifndef BLE_CONN_TIMEOUT
#define BLE_CONN_TIMEOUT 600  // 10ms units → 6 s supervision
#endif
#ifndef BLE_CONNECT_TIMEOUT_S
#define BLE_CONNECT_TIMEOUT_S 5
#endif

// ---------------------------------------------------------------------------
// EPEVER MPPT via HN_xxxx BLE gateway (Modbus RTU over GATT 0x2B00)
// ---------------------------------------------------------------------------
#ifndef MPPT_ENABLE
#define MPPT_ENABLE 1
#endif
// MAC of the HN_xxxx gateway (its BLE name without "HN_"). Empty "" = MPPT off.
#ifndef MPPT_MAC
#define MPPT_MAC ""
#endif
#ifndef MPPT_MODBUS_ADDR
#define MPPT_MODBUS_ADDR 1
#endif
// One full read cycle (5 short Modbus requests) every N ms.
#ifndef MPPT_POLL_MS
#define MPPT_POLL_MS 10000UL
#endif
#ifndef MPPT_POLL_SLEEP_MS
#define MPPT_POLL_SLEEP_MS 30000UL
#endif
#ifndef MPPT_STALE_MS
#define MPPT_STALE_MS 75000UL
#endif
#ifndef MPPT_REPLY_TIMEOUT_MS
#define MPPT_REPLY_TIMEOUT_MS 2000UL
#endif
// This many cycles without a single valid reply → drop and re-establish the link.
#ifndef MPPT_MAX_DEAD_CYCLES
#define MPPT_MAX_DEAD_CYCLES 3
#endif

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

// UI language: UI_LANG_EN or UI_LANG_RU.
#define UI_LANG_EN 0
#define UI_LANG_RU 1
#ifndef UI_LANG
#define UI_LANG UI_LANG_EN
#endif
#ifndef UI_REFRESH_MS
#define UI_REFRESH_MS 1000UL
#endif
#ifndef TZ_INFO
#define TZ_INFO "UTC0"  // POSIX TZ for the NTP clock, e.g. "CET-1CEST,M3.5.0,M10.5.0/3"
#endif
#ifndef NTP_SERVER
#define NTP_SERVER "pool.ntp.org"
#endif
// "Grid off since": the outage starts when the bank switches to discharge; it counts
// only once total discharge current exceeds this at least once in that episode.
#ifndef LIGHT_OUTAGE_MIN_A
#define LIGHT_OUTAGE_MIN_A 1.0f
#endif
// The MPPT charge current is subtracted first (solar is not "light"). BMS and
// MPPT are sampled at different moments, so the outage must hold this long to
// count, and must be gone this long to end (the start time is still exact).
#ifndef LIGHT_START_MS
#define LIGHT_START_MS 20000UL
#endif
#ifndef LIGHT_END_MS
#define LIGHT_END_MS 30000UL
#endif
// Current colour thresholds (|A|): yellow / red. Set them to your BMS and
// wiring ratings; the bank totals default to pack values x JK_BMS_COUNT.
#ifndef PACK_CUR_WARN_A
#define PACK_CUR_WARN_A 50.0f
#endif
#ifndef PACK_CUR_BAD_A
#define PACK_CUR_BAD_A 80.0f
#endif
#ifndef TOTAL_CUR_WARN_A
#define TOTAL_CUR_WARN_A (PACK_CUR_WARN_A * JK_BMS_COUNT)
#endif
#ifndef TOTAL_CUR_BAD_A
#define TOTAL_CUR_BAD_A (PACK_CUR_BAD_A * JK_BMS_COUNT)
#endif

// No touches for this long → back to the main screen.
#ifndef UI_IDLE_HOME_MS
#define UI_IDLE_HOME_MS 30000UL
#endif
// Screen sleep: backlight off after this long without touch (0 = never).
#ifndef UI_SLEEP_MS
#define UI_SLEEP_MS 60000UL
#endif
// Backlight level while asleep, 0..255 (0 = off). Awake = 255.
#ifndef UI_SLEEP_BRIGHTNESS
#define UI_SLEEP_BRIGHTNESS 0
#endif
// Screensaver instead of a dark screen, local time (TZ_INFO) from
// SAVER_FROM_H:00 to SAVER_TO_H:00 (wraps midnight; 0..24 = round the clock).
// Outside it, or without NTP time, the panel is switched off.
// SAVER_FROM_H == SAVER_TO_H = never.
#ifndef SAVER_FROM_H
#define SAVER_FROM_H 0
#endif
#ifndef SAVER_TO_H
#define SAVER_TO_H 24
#endif
// Scene start hours by season {morning, day, evening, late night}; late runs until morning.
// Rows: winter (Dec-Feb), spring (Mar-May), summer (Jun-Aug), autumn (Sep-Nov).
// Defaults suit mid-northern latitudes; adjust to your daylight.
#ifndef SAVER_SEASON_HOURS
#define SAVER_SEASON_HOURS {{8, 12, 16, 21}, {7, 12, 19, 1}, {7, 11, 20, 1}, {7, 12, 17, 22}}
#endif
// Hold a finger this long to play the screensaver for SAVER_DEMO_MS (any hour).
#ifndef SAVER_HOLD_MS
#define SAVER_HOLD_MS 5000UL
#endif
#ifndef SAVER_DEMO_MS
#define SAVER_DEMO_MS 10000UL
#endif
#ifndef SAVER_FRAME_MS
#define SAVER_FRAME_MS 420UL  // one sprite-sheet frame
#endif
#ifndef SAVER_BRIGHTNESS
#define SAVER_BRIGHTNESS 160
#endif
// Weather overlay on the screensaver (Open-Meteo, no API key, plain HTTP).
// Empty WEATHER_LAT / WEATHER_LON = no weather requests.
#ifndef WEATHER_ENABLE
#define WEATHER_ENABLE 1
#endif
#ifndef WEATHER_LAT
#define WEATHER_LAT ""
#endif
#ifndef WEATHER_LON
#define WEATHER_LON ""
#endif
#ifndef WEATHER_PERIOD_MS
#define WEATHER_PERIOD_MS 900000UL  // refresh every 15 min
#endif
#ifndef WEATHER_STALE_MS
#define WEATHER_STALE_MS 10800000UL  // drop the effect after 3 h without data
#endif
#ifndef WEATHER_FORCE
#define WEATHER_FORCE -1  // >=0: always show that WeatherFx (testing)
#endif
// Weather tour (hold 5 s, then tap within the demo): each effect shown this long.
#ifndef SAVER_TOUR_STEP_MS
#define SAVER_TOUR_STEP_MS 5000UL
#endif
// BMS decode interval while the screen sleeps (UDP metrics still go out).
#ifndef BMS_POLL_SLEEP_MS
#define BMS_POLL_SLEEP_MS 15000UL
#endif

// ---------------------------------------------------------------------------
// Wi-Fi, metrics
// ---------------------------------------------------------------------------
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef WIFI_HOSTNAME
#define WIFI_HOSTNAME "cyd-jk-bms"
#endif
#ifndef HTTP_METRICS_PORT
#define HTTP_METRICS_PORT 80
#endif
// CPU clock, MHz (240 / 160 / 80). 160 is enough for LVGL + Wi-Fi + BLE
// and runs cooler on less current than 240. Wi-Fi/BLE need at least 80.
#ifndef CPU_FREQ_MHZ
#define CPU_FREQ_MHZ 160
#endif
// Wi-Fi transmit power (wifi_power_t). 19.5 dBm is the maximum; 15 dBm uses
// noticeably less current. Raise it back if the link to the router gets weak.
#ifndef WIFI_TX_POWER
#define WIFI_TX_POWER WIFI_POWER_15dBm
#endif
#ifndef WIFI_CONNECT_TIMEOUT_MS
#define WIFI_CONNECT_TIMEOUT_MS 25000UL
#endif
// CYD pushes a JSON metrics datagram on this cadence (UDP broadcast).
// Collector just listens — no polling load on the ESP32.
#ifndef METRICS_PUSH_MS
#define METRICS_PUSH_MS 15000UL
#endif
#ifndef METRICS_UDP_PORT
#define METRICS_UDP_PORT 9522
#endif

// ---------------------------------------------------------------------------
// Diagnostics thresholds
// ---------------------------------------------------------------------------

// |I| below this is "idle" (no charge/discharge, no ETA)
#ifndef DISCHARGE_ETA_MIN_A
#define DISCHARGE_ETA_MIN_A 0.2f
#endif

// Cell dV traffic-light — only valid at rest + high SOC (LFP top of plateau)
#ifndef DV_REST_MAX_A
#define DV_REST_MAX_A 0.5f          // |I| below → treat as rest (no time history)
#endif
#ifndef DV_GRADE_MIN_SOC
#define DV_GRADE_MIN_SOC 90.0f      // below mid-plateau grading is meaningless
#endif
#ifndef DV_ABSORB_CELL_V
#define DV_ABSORB_CELL_V 3.45f      // active top-balance peak, not a defect
#endif
#ifndef DV_GREEN_MV
#define DV_GREEN_MV 20.0f
#endif
#ifndef DV_YELLOW_MV
#define DV_YELLOW_MV 50.0f
#endif
// Imbalance this large inside the flat LFP plateau is a warning in any state.
#ifndef DV_HARD_ALARM_MV
#define DV_HARD_ALARM_MV 150.0f
#endif
#ifndef DV_PLATEAU_MIN_V
#define DV_PLATEAU_MIN_V 3.10f
#endif

// Temperature warnings (°C)
#ifndef TEMP_CELL_WARN_HIGH_C
#define TEMP_CELL_WARN_HIGH_C 50.0f
#endif
#ifndef TEMP_MOS_WARN_HIGH_C
#define TEMP_MOS_WARN_HIGH_C 70.0f
#endif
#ifndef TEMP_CELL_WARN_LOW_C
#define TEMP_CELL_WARN_LOW_C 0.0f   // only while charging (LFP plating)
#endif

// ---------------------------------------------------------------------------
// CYD touch (XPT2046 on VSPI)
// ---------------------------------------------------------------------------
#define CYD_TOUCH_MOSI 32
#define CYD_TOUCH_MISO 39
#define CYD_TOUCH_CLK  25
#define CYD_TOUCH_CS   33
#define CYD_TOUCH_IRQ  36
#define CYD_TFT_BL     21

static_assert(JK_BMS_COUNT >= 1 && JK_BMS_COUNT <= 2, "JK_BMS_COUNT must be 1 or 2");
static_assert(STALE_TIMEOUT_MS > BMS_POLL_MS, "STALE_TIMEOUT_MS must exceed BMS_POLL_MS");
