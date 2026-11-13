#include "screensaver.h"

#include <string.h>

#include "config.h"
#include "saver_frames.h"
#include "saver_fx.h"
#include "weather.h"

namespace {

constexpr int W = SAVER_W;
constexpr int H = SAVER_H;

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// 5x7 glyphs, one byte per row (low 5 bits, bit4 = left): 0-9, ':', '%'
const uint8_t GLYPH[12][7] = {
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E},
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C},
    {0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00}, {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03},
};

int glyph_of(char ch) {
  if (ch >= '0' && ch <= '9') return ch - '0';
  if (ch == ':') return 10;
  if (ch == '%') return 11;
  return -1;  // space
}

// HUD at panel resolution (2x the scene): 5x7 glyphs stretched to 12x16.
constexpr int PW = W * 2;
constexpr int GW = 12, GH = 16, ADV = GW + 2;
constexpr int PAD_X = 6, TEXT_Y = 4, PLATE_H = GH + 2 * TEXT_Y;

}  // namespace

void saver_render(uint16_t *fb, int y0, int rows, int scene, uint32_t t) {
  if (scene < 0 || scene >= SAVER_SCENE_COUNT) scene = SAVER_NIGHT;
  const SaverSceneData &sc = SAVER_SCENES[scene];
  // Frame = key frame + its patch (spans of pixels that differ from the key),
  // clipped to the band.
  const uint32_t f = (t / SAVER_FRAME_MS) % SAVER_FRAMES;
  const uint32_t lo = (uint32_t)y0 * W, hi = lo + (uint32_t)rows * W;
  memcpy(fb, sc.key + lo, (hi - lo) * sizeof(uint16_t));
  for (uint32_t p = sc.at[f]; p < sc.at[f + 1];) {
    const uint32_t off = sc.patch[p], n = sc.patch[p + 1];
    const uint32_t a = off > lo ? off : lo, b = off + n < hi ? off + n : hi;
    if (a < b) memcpy(fb + (a - lo), &sc.patch[p + 2 + (a - off)], (b - a) * sizeof(uint16_t));
    p += 2u + n;
  }
}

void saver_hud_line(uint16_t *line, int y, const char *hud) {
  if (!hud || !*hud || y >= PLATE_H) return;
  const int n = (int)strlen(hud);
  const int tw = n * ADV - 2;
  const int x0 = PW - tw - 2 * PAD_X;  // plate: x0..PW, rows 0..PLATE_H
  // 50% black plate: halve every channel.
  for (int x = x0 < 0 ? 0 : x0; x < PW; x++) line[x] = (uint16_t)((line[x] >> 1) & 0x7BEF);
  const int gy = y - TEXT_Y;
  if (gy < 0 || gy >= GH) return;
  const int row = gy * 7 / GH;
  const uint16_t c = rgb(235, 245, 255);
  int x = x0 + PAD_X;
  for (int i = 0; i < n; i++, x += ADV) {
    const int g = glyph_of(hud[i]);
    if (g < 0) continue;
    const uint8_t bits = GLYPH[g][row];
    for (int gx = 0; gx < GW; gx++)
      if (bits & (0x10 >> (gx * 5 / GW)) && (unsigned)(x + gx) < (unsigned)PW) line[x + gx] = c;
  }
}

// ---------------------------------------------------------------------------
// Weather overlay from generated sheets (include/saver_fx.h): per-pixel
// intensity 0..7 blended toward one tint. RLE is skipped from the frame start
// for each band, so no state is kept between bands.
// ---------------------------------------------------------------------------
namespace {

// al: 0..32 share of b.
inline uint16_t mix(uint16_t a, uint16_t b, int al) {
  const int ia = 32 - al;
  const int r = ((a >> 11) * ia + (b >> 11) * al) >> 5;
  const int g = (((a >> 5) & 63) * ia + ((b >> 5) & 63) * al) >> 5;
  const int bl = ((a & 31) * ia + (b & 31) * al) >> 5;
  return (uint16_t)(r << 11 | g << 5 | bl);
}

constexpr uint32_t FX_FLASH_MS = 8000;        // lightning once per this period
constexpr uint32_t FX_FLASH_FRAME_MS = 450;   // each lightning frame held this long

void fx_dim(uint16_t *fb, int rows, int al) {
  for (int i = 0; i < rows * W; i++) fb[i] = mix(fb[i], 0, al);
}

}  // namespace

uint32_t saver_fx_period(int fx) {
  if (fx <= WX_NONE || fx >= WX_COUNT) return 0;
  return SAVER_FX[fx - 1].frame_ms;
}

void saver_weather(uint16_t *fb, int y0, int rows, int fx, uint32_t t) {
  if (fx <= WX_NONE || fx >= WX_COUNT) return;
  const SaverFxData &d = SAVER_FX[fx - 1];
  if (fx == WX_RAIN) fx_dim(fb, rows, 4);
  if (fx == WX_STORM) fx_dim(fb, rows, 7);
  int f;
  const uint32_t ph = t % FX_FLASH_MS;
  if (d.n_flash && ph < (uint32_t)d.n_flash * FX_FLASH_FRAME_MS)
    f = d.seq[d.n_normal + ph / FX_FLASH_FRAME_MS];
  else
    f = d.seq[(t / d.frame_ms) % d.n_normal];
  const int sc = d.scale, lw = W / sc;
  const uint8_t *p = d.data + d.at[f], *end = d.data + d.at[f + 1];
  // pixel index in the low-res grid where this band starts / ends
  const int a = (y0 / sc) * lw, b = ((y0 + rows - 1) / sc + 1) * lw;
  int pos = 0;
  while (p < end && pos < b) {
    const uint8_t c = *p++;
    int n, v;
    if (c < 0x80) { n = c + 1; v = 0; } else { n = (c & 15) + 1; v = (c >> 4) & 7; }
    if (v && pos + n > a) {
      const int al = v * d.gain > 32 ? 32 : v * d.gain;  // >32 overflows mix()
      for (int k = pos < a ? a : pos, e = pos + n < b ? pos + n : b; k < e; k++) {
        const int lx = k % lw, ly = k / lw;
        for (int dy = 0; dy < sc; dy++) {
          const int y = ly * sc + dy - y0;
          if (y < 0 || y >= rows) continue;
          uint16_t *q = fb + y * W + lx * sc;
          for (int dx = 0; dx < sc; dx++) q[dx] = mix(q[dx], d.tint, al);
        }
      }
    }
    pos += n;
  }
}
