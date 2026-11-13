#pragma once
#include <stdint.h>

// Current weather, reduced to what the screensaver can animate.
enum WeatherFx { WX_NONE = 0, WX_FOG, WX_DRIZZLE, WX_RAIN, WX_SNOW, WX_STORM, WX_COUNT };

// Maps a WMO weather code (Open-Meteo "weather_code") to an effect.
int weather_fx_from_wmo(int code);

// Called from the network task: fetches Open-Meteo every WEATHER_PERIOD_MS.
void weather_loop(bool wifi_ok);

// Effect to draw now (WX_NONE when disabled, unknown or stale).
int weather_fx();
