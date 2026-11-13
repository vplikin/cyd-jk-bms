#pragma once

void ui_init();
// Screen sleep support: background tick while asleep, refresh on wake.
void ui_sleep_tick();
void ui_wake();
// Screensaver allowed now: SAVER_FROM_H..SAVER_TO_H local time (needs NTP).
bool ui_saver_hours();
// Screensaver scene for the current hour (SaverScene); night without NTP time.
int ui_saver_scene();
// Corner text for the screensaver: "HH:MM NN%".
void ui_saver_hud(char *buf, unsigned n);
