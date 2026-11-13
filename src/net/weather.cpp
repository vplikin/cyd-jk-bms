#include "weather.h"

#include "config.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <stdlib.h>
#include <string.h>

static volatile int g_fx = WX_NONE;
static volatile uint32_t g_fx_ms = 0;  // millis() of the last good reply (0 = never)
static uint32_t g_next_ms = 5000;      // first try shortly after boot

int weather_fx_from_wmo(int c) {
  if (c == 45 || c == 48) return WX_FOG;
  if (c >= 51 && c <= 57) return WX_DRIZZLE;
  if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return WX_RAIN;
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return WX_SNOW;
  if (c >= 95 && c <= 99) return WX_STORM;
  return WX_NONE;  // clear / cloudy: the scene already has a sky
}

static bool fetch() {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(4000);
  http.setConnectTimeout(4000);
  const char *url = "http://api.open-meteo.com/v1/forecast?latitude=" WEATHER_LAT
                    "&longitude=" WEATHER_LON "&current=weather_code";
  if (!http.begin(client, url)) return false;
  const int st = http.GET();
  bool ok = false;
  if (st == 200) {
    String body = http.getString();
    // {"current_units":{...,"weather_code":"wmo code"},"current":{...,"weather_code":61}}
    int at = body.indexOf("\"current\":");
    if (at >= 0) at = body.indexOf("\"weather_code\":", at);
    if (at >= 0) {
      const int code = atoi(body.c_str() + at + 15);
      g_fx = weather_fx_from_wmo(code);
      g_fx_ms = millis() | 1;
      ok = true;
      Serial.printf("weather: wmo=%d fx=%d\n", code, g_fx);
    }
  } else {
    Serial.printf("weather: http %d\n", st);
  }
  http.end();
  return ok;
}

void weather_loop(bool wifi_ok) {
#if WEATHER_ENABLE
  const uint32_t now = millis();
  if (!WEATHER_LAT[0] || !WEATHER_LON[0]) return;  // no location configured
  if (!wifi_ok || (int32_t)(now - g_next_ms) < 0) return;
  g_next_ms = now + (fetch() ? WEATHER_PERIOD_MS : 120000UL);  // retry in 2 min
#else
  (void)wifi_ok;
#endif
}

int weather_fx() {
#if WEATHER_FORCE >= 0
  return WEATHER_FORCE;
#else
  if (!WEATHER_ENABLE || g_fx_ms == 0 || millis() - g_fx_ms > WEATHER_STALE_MS) return WX_NONE;
  return g_fx;
#endif
}
