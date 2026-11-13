#pragma once
#include <stdint.h>

// Pixel-art scene in four variants (morning / day / night / late), 8 frames each,
// stored as a key frame + patches (include/saver_frames.h, tools/pack_saver.py).
// Low-res framebuffer, shown 2x scaled on the 320x240 panel.
#define SAVER_W 160
#define SAVER_H 120

// Scene order matches tools/pack_saver.py; a scene missing from the build renders as night.
enum SaverScene { SAVER_MORNING = 0, SAVER_DAY = 1, SAVER_NIGHT = 2, SAVER_LATE = 3 };

// Renders rows [y0, y0+rows) of a scene into fb (SAVER_W*rows RGB565). t_ms: animation time.
void saver_render(uint16_t *fb, int y0, int rows, int scene, uint32_t t_ms);

// Weather overlay (WeatherFx from weather.h) on top of a rendered band.
// Frame period of an effect (0 = none).
uint32_t saver_fx_period(int fx);
void saver_weather(uint16_t *fb, int y0, int rows, int fx, uint32_t t_ms);

// Draws the HUD (digits, ':', '%', ' ') into one panel-resolution line
// (SAVER_W*2 px, panel row y): top-right corner on a 50% dark plate.
void saver_hud_line(uint16_t *line, int y, const char *hud);
