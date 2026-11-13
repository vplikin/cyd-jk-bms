#include "cyd_display.h"
#include "config.h"
#include "jk_bms_client.h"
#include "screensaver.h"
#include "weather.h"
#include "ui.h"

#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

static TFT_eSPI tft = TFT_eSPI();
static SPIClass touchSpi = SPIClass(VSPI);
static XPT2046_Touchscreen ts(CYD_TOUCH_CS, CYD_TOUCH_IRQ);

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = nullptr;
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;
static lv_disp_t *disp = nullptr;

static bool g_asleep = false;
static bool g_swallow = false;  // wake touch: ignore until released
static uint32_t g_sleep_tick_ms = 0;
static bool g_saver = false;         // asleep, but showing the animation
// Saver is drawn in bands: 8 rows x 160 px = 2.5 KB instead of a 38 KB frame.
static constexpr int SAVER_BAND = 8;
static uint16_t g_saver_band[SAVER_W * SAVER_BAND];
static uint32_t g_saver_frame_ms = 0;
static uint32_t g_saver_t0 = 0;
static uint32_t g_press_ms = 0;       // when the current touch began (0 = none)
static bool g_demo_req = false;       // long press detected
static uint32_t g_demo_until = 0;     // forced screensaver end (0 = not a demo)
static bool g_wait_release = false;   // ignore the finger that started the demo
static uint32_t g_tour_t0 = 0;        // weather tour start (0 = off): tap during the demo

static void backlight(uint8_t level) { analogWrite(CYD_TFT_BL, level); }

static const uint16_t touch_x_min = 200;
static const uint16_t touch_x_max = 3700;
static const uint16_t touch_y_min = 240;
static const uint16_t touch_y_max = 3800;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t *)&color_p->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(drv);
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
  (void)drv;
  const bool down = ts.tirqTouched() && ts.touched();
  if (g_swallow) {
    if (!down) g_swallow = false;
    data->state = LV_INDEV_STATE_REL;
    return;
  }
  if (down) {
    const uint32_t now = millis();
    if (g_press_ms == 0) g_press_ms = now ? now : 1;
    else if (now - g_press_ms >= SAVER_HOLD_MS) {
      g_demo_req = true;
      g_press_ms = 0;
      g_swallow = true;  // release of this press must not flip a page
      data->state = LV_INDEV_STATE_REL;
      return;
    }
    TS_Point p = ts.getPoint();
    int16_t x = map(p.x, touch_x_min, touch_x_max, 0, tft.width() - 1);
    int16_t y = map(p.y, touch_y_min, touch_y_max, 0, tft.height() - 1);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= tft.width()) x = tft.width() - 1;
    if (y >= tft.height()) y = tft.height() - 1;
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
  } else {
    g_press_ms = 0;
    data->state = LV_INDEV_STATE_REL;
  }
}

void cyd_display_init() {
  pinMode(CYD_TFT_BL, OUTPUT);
  digitalWrite(CYD_TFT_BL, HIGH);

  tft.init();  // also drives TFT_BL as plain GPIO
  tft.setRotation(1);  // 320x240 landscape
  tft.fillScreen(TFT_BLACK);
  backlight(255);  // hand the pin to LEDC only after TFT_eSPI is done with it

  touchSpi.begin(CYD_TOUCH_CLK, CYD_TOUCH_MISO, CYD_TOUCH_MOSI, CYD_TOUCH_CS);
  ts.begin(touchSpi);
  ts.setRotation(1);

  lv_init();

  const uint32_t buf_pixels = 320 * 16;
  buf1 = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * buf_pixels, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!buf1) {
    buf1 = (lv_color_t *)malloc(sizeof(lv_color_t) * buf_pixels);
  }
  lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, buf_pixels);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = 320;
  disp_drv.ver_res = 240;
  disp_drv.flush_cb = flush_cb;
  disp_drv.draw_buf = &draw_buf;
  disp = lv_disp_drv_register(&disp_drv);

  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read_cb;
  lv_indev_drv_register(&indev_drv);
}

static void panel_off() {
  backlight(UI_SLEEP_BRIGHTNESS);
#if UI_SLEEP_BRIGHTNESS == 0
  tft.writecommand(0x10);  // ILI9341 SLPIN: panel off, GRAM kept
#endif
}

static void panel_on() {
#if UI_SLEEP_BRIGHTNESS == 0
  tft.writecommand(0x11);  // SLPOUT
  delay(120);              // datasheet: 120 ms before the next command
#endif
}

static bool saver_start() {
  g_saver = true;
  g_saver_t0 = millis();
  g_saver_frame_ms = 0;
  Serial.println("display: screensaver");
  return true;
}

static void saver_stop() {
  g_saver = false;
}

// Tour shows every effect once for SAVER_TOUR_STEP_MS; otherwise the live weather.
static int saver_fx_now() {
  if (!g_tour_t0) return weather_fx();
  const int i = 1 + (int)((millis() - g_tour_t0) / SAVER_TOUR_STEP_MS);
  return i < WX_COUNT ? i : WX_COUNT - 1;
}

// Weather animation needs a faster refresh than the scene alone.
static uint32_t saver_period() {
  const uint32_t p = saver_fx_period(saver_fx_now());
  return p ? p : SAVER_FRAME_MS;
}

static void saver_frame(uint32_t now) {
  char hud[16];
  ui_saver_hud(hud, sizeof(hud));
  const int scene = ui_saver_scene();
  const int fx = saver_fx_now();
  static uint16_t line[SAVER_W * 2];
  tft.startWrite();
  tft.setAddrWindow(0, 0, SAVER_W * 2, SAVER_H * 2);
  for (int y = 0; y < SAVER_H; y++) {
    if (y % SAVER_BAND == 0) {
      saver_render(g_saver_band, y, SAVER_BAND, scene, now - g_saver_t0);
      saver_weather(g_saver_band, y, SAVER_BAND, fx, now - g_saver_t0);
    }
    const uint16_t *src = g_saver_band + (y % SAVER_BAND) * SAVER_W;
    for (int r = 0; r < 2; r++) {  // each scene row -> two panel rows
      for (int x = 0; x < SAVER_W; x++) line[2 * x] = line[2 * x + 1] = src[x];
      saver_hud_line(line, 2 * y + r, hud);
      tft.pushColors(line, SAVER_W * 2, true);  // pushColors swaps in place
    }
  }
  tft.endWrite();
  if (g_saver_frame_ms == 0) backlight(SAVER_BRIGHTNESS);  // first frame drawn
}

static void enter_sleep() {
  g_asleep = true;
  jk_bms_set_sleep(true);
  if (ui_saver_hours() && saver_start()) return;  // panel stays on
  panel_off();
  Serial.println("display: sleep");
}

static void wake() {
  g_demo_until = 0;
  g_wait_release = false;
  g_tour_t0 = 0;
  if (g_saver) saver_stop();
  else panel_on();
  g_asleep = false;
  g_swallow = true;  // this touch only wakes, it doesn't flip pages
  jk_bms_set_sleep(false);
  lv_disp_trig_activity(disp);
  ui_wake();           // also invalidates the screen (GRAM held the saver)
  lv_timer_handler();  // redraw before the light comes on
  backlight(255);
  Serial.println("display: wake");
}

void cyd_display_loop() {
  if (g_asleep) {
    const uint32_t now = millis();
    // Touch IRQ still works with the panel asleep.
    const bool down = ts.tirqTouched() && ts.touched();
    if (g_wait_release) {
      if (!down) g_wait_release = false;
    } else if (down && g_demo_until && !g_tour_t0) {
      // Tap within the demo: tour all weather effects once, then wake.
      g_tour_t0 = now ? now : 1;
      g_demo_until = g_tour_t0 + (WX_COUNT - 1) * SAVER_TOUR_STEP_MS;
      if (g_demo_until == 0) g_demo_until = 1;
      g_wait_release = true;
      g_saver_frame_ms = now - 100000;  // draw right away
      Serial.println("display: weather tour");
    } else if (down) {
      wake();
      return;
    }
    if (g_demo_until) {
      if ((int32_t)(now - g_demo_until) >= 0) {
        wake();
        return;
      }
      if (now - g_saver_frame_ms >= saver_period()) {
        saver_frame(now);
        g_saver_frame_ms = now;
      }
      return;  // demo ignores the schedule
    }
    if ((int32_t)(now - g_sleep_tick_ms) >= (int32_t)UI_REFRESH_MS) {
      g_sleep_tick_ms = now;
      ui_sleep_tick();
      // Follow the schedule (SAVER_FROM_H..SAVER_TO_H; also NTP lost/regained).
      const bool want = ui_saver_hours();
      if (g_saver && !want) {
        saver_stop();
        panel_off();
        Serial.println("display: saver -> sleep");
      } else if (!g_saver && want) {
        panel_on();
        if (!saver_start()) panel_off();
      }
    }
    if (g_saver && (now - g_saver_frame_ms) >= saver_period()) {
      saver_frame(now);
      g_saver_frame_ms = now;
    }
    return;
  }
  lv_timer_handler();
  if (g_demo_req) {
    g_demo_req = false;
    if (saver_start()) {
      g_asleep = true;
      g_wait_release = true;
      g_demo_until = millis() + SAVER_DEMO_MS;
      if (g_demo_until == 0) g_demo_until = 1;
      Serial.println("display: screensaver demo");
    }
    return;
  }
#if UI_SLEEP_MS > 0
  if (lv_disp_get_inactive_time(disp) >= UI_SLEEP_MS) enter_sleep();
#endif
}

lv_disp_t *cyd_disp() { return disp; }
