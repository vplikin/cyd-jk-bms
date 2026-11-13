// 320x240 landscape UI.
//   y 0..22   status bar (always visible): link state of every BMS, WiFi, page
//   y 24..240 page: 0 = system summary, 1..N = per-pack details, last = MPPT
// Tap anywhere → next page. No touch for UI_IDLE_HOME_MS → page 0.
#include "ui.h"
#include "screensaver.h"

#include <Arduino.h>
#include <lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "battery_state.h"
#include "config.h"
#include "i18n.h"
#include "fonts_ru.h"
#include "http_metrics.h"
#include "jk_alarms.h"
#include "mppt_state.h"
#include "pack_summary.h"
#include "persist.h"

namespace {

// The MPPT page and status icon exist only when a gateway MAC is configured.
constexpr bool kHasMppt = MPPT_ENABLE && sizeof(MPPT_MAC) > 1;
constexpr uint8_t kPages = 1 + JK_BMS_COUNT + (kHasMppt ? 1 : 0);
constexpr uint8_t kMpptPage = kHasMppt ? kPages - 1 : 0xFF;
constexpr lv_coord_t kBarH = 22;
constexpr lv_coord_t kPageY = 24;
constexpr lv_coord_t kPageH = 240 - kPageY;

const lv_color_t C_BG = lv_color_hex(0x0F1419);
const lv_color_t C_TEXT = lv_color_hex(0xE8EEF5);
const lv_color_t C_MUTED = lv_color_hex(0x8FA3B8);
const lv_color_t C_SEP = lv_color_hex(0x2A3540);
const lv_color_t C_OK = lv_color_hex(0x3DDC97);
const lv_color_t C_WARN = lv_color_hex(0xF4A261);
const lv_color_t C_BAD = lv_color_hex(0xE76F51);
const lv_color_t C_YEL = lv_color_hex(0xFFD54F);

struct PackPage {
  lv_obj_t *root;
  lv_obj_t *header;
  lv_obj_t *grid;
  lv_obj_t *soh, *energy, *cap, *temp, *cells, *dv, *mos, *alarm, *ble, *wifi;
};

struct MpptPage {
  lv_obj_t *header, *grid;
  lv_obj_t *pv, *bat, *stage, *temp, *state, *ble, *age;
};

struct Ui {
  uint8_t page = 0;
  lv_obj_t *pages[kPages];

  // status bar
  lv_obj_t *st_pack[JK_BMS_COUNT];
  lv_obj_t *st_pv;
  lv_obj_t *st_wifi;
  lv_obj_t *st_page;

  // main page
  lv_obj_t *soc, *soc_pct, *mode, *eta, *energy, *light, *solar;
  // "Grid off since": outage episode = the bank is discharging net of the MPPT
  // (grid-side current < 0). It counts once that drew > LIGHT_OUTAGE_MIN_A
  // for LIGHT_START_MS; it ends after LIGHT_END_MS without net discharge.
  bool light_seen = false;     // saw CHARGE/IDLE since boot
  bool out_episode = false;    // currently in a DISCHARGE stretch
  bool out_confirmed = false;  // ...and it drew > LIGHT_OUTAGE_MIN_A
  bool out_known = false;      // out_ms is a real switch moment (not boot)
  uint32_t out_ms = 0;         // millis() when DISCHARGE began
  uint32_t calm_ms = 0;        // millis() since net discharge stopped (0 = discharging)
  time_t out_t = 0;            // wall-clock outage start (persisted in NVS)
  bool saved_loaded = false;
  time_t saved_t = 0;          // value read from NVS at boot
  lv_obj_t *warn_box, *warn_icon, *warn_text;
  lv_obj_t *cur, *volt, *pwr, *clock;

  PackPage pp[JK_BMS_COUNT];
  MpptPage mp;
} ui;

lv_style_t st_box;  // transparent, borderless, non-scrolling container

// ---- change-only setters (avoid needless invalidation / redraw) ----------

void set_text(lv_obj_t *l, const char *t) {
  const char *cur = lv_label_get_text(l);
  if (cur && strcmp(cur, t) == 0) return;
  lv_label_set_text(l, t);
}

void set_color(lv_obj_t *o, lv_color_t c) {
  if (lv_obj_get_style_text_color(o, LV_PART_MAIN).full == c.full) return;
  lv_obj_set_style_text_color(o, c, 0);
}

void set_hidden(lv_obj_t *o, bool hidden) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == hidden) return;
  if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void set_opa(lv_obj_t *o, lv_opa_t opa) {
  if (lv_obj_get_style_opa(o, LV_PART_MAIN) == opa) return;
  lv_obj_set_style_opa(o, opa, 0);
}

// ---- builders ------------------------------------------------------------

lv_obj_t *box(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_add_style(o, &st_box, 0);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  return o;
}

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                lv_color_t color = C_TEXT, lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_obj_set_style_text_align(l, align, 0);
  lv_label_set_text(l, "--");
  lv_obj_set_pos(l, x, y);
  if (w > 0) {
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
  }
  return l;
}

void hline(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w) {
  lv_obj_t *o = box(parent, x, y, w, 1);
  lv_obj_set_style_bg_color(o, C_SEP, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

void vline(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t h) {
  lv_obj_t *o = box(parent, x, y, 1, h);
  lv_obj_set_style_bg_color(o, C_SEP, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

// "Title" (muted) + big number + small unit on one baseline. Returns number.
lv_obj_t *metric(lv_obj_t *parent, const char *title, const char *unit, lv_coord_t x, lv_coord_t y,
                 lv_coord_t w) {
  lv_obj_t *t = label(parent, &font_ru_14, x, y, w, C_MUTED);
  lv_label_set_text(t, title);

  lv_obj_t *row = box(parent, x, y + 18, w, 34);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  lv_obj_set_style_pad_column(row, 6, 0);

  lv_obj_t *num = lv_label_create(row);
  lv_obj_set_style_text_font(num, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(num, C_TEXT, 0);
  lv_label_set_text(num, "--");

  lv_obj_t *u = lv_label_create(row);
  lv_obj_set_style_text_font(u, &font_ru_16, 0);
  lv_obj_set_style_text_color(u, C_MUTED, 0);
  lv_obj_set_style_pad_bottom(u, 3, 0);
  lv_label_set_text(u, unit);
  return num;
}

// Detail-grid cell: title (muted, 14) + value (16). Returns value label.
lv_obj_t *cell(lv_obj_t *parent, const char *title, uint8_t col, uint8_t row) {
  const lv_coord_t x = col ? 164 : 8;
  const lv_coord_t y = 6 + row * 37;
  lv_obj_t *t = label(parent, &font_ru_14, x, y, 148, C_MUTED);
  lv_label_set_text(t, title);
  lv_obj_t *v = label(parent, &font_ru_16, x, y + 16, 148);
  lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
  return v;
}

// Status-bar sun: one object that paints a disc + 8 rays itself (the fonts
// have no sun glyph, and the LVGL pool has no room for a child per ray).
lv_color_t g_sun_color;

void sun_draw(lv_event_t *e) {
  lv_obj_t *o = lv_event_get_target(e);
  lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = g_sun_color;
  // {x, y, w, h}: disc, then rays N S W E and the four diagonals
  static const uint8_t parts[][4] = {
      {5, 5, 7, 7},  {7, 0, 3, 3},  {7, 14, 3, 3}, {0, 7, 3, 3}, {14, 7, 3, 3},
      {2, 2, 2, 2},  {13, 2, 2, 2}, {2, 13, 2, 2}, {13, 13, 2, 2},
  };
  for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
    d.radius = i == 0 ? LV_RADIUS_CIRCLE : 1;
    const lv_area_t r = {(lv_coord_t)(a.x1 + parts[i][0]), (lv_coord_t)(a.y1 + parts[i][1]),
                         (lv_coord_t)(a.x1 + parts[i][0] + parts[i][2] - 1),
                         (lv_coord_t)(a.y1 + parts[i][1] + parts[i][3] - 1)};
    lv_draw_rect(ctx, &d, &r);
  }
}

void sun_set_color(lv_obj_t *sun, lv_color_t col) {
  if (lv_color_to16(col) == lv_color_to16(g_sun_color)) return;
  g_sun_color = col;
  lv_obj_invalidate(sun);
}

lv_obj_t *sun_icon(lv_obj_t *parent, lv_coord_t x, lv_coord_t y) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_remove_style_all(c);
  lv_obj_set_pos(c, x, y);
  lv_obj_set_size(c, 17, 17);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
  g_sun_color = C_MUTED;
  lv_obj_add_event_cb(c, sun_draw, LV_EVENT_DRAW_MAIN, nullptr);
  return c;
}

void build_status_bar(lv_obj_t *scr) {
  for (uint8_t i = 0; i < JK_BMS_COUNT; i++) {
    ui.st_pack[i] = label(scr, &font_ru_14, 6 + i * 106, 3, 102, C_MUTED);
  }
  // Solar charger link: a small sun (disc + 8 rays), coloured like the pack links.
  if (kHasMppt) ui.st_pv = sun_icon(scr, 224, 3);
  ui.st_wifi = label(scr, &lv_font_montserrat_14, 254, 4, 18, C_BAD);
  lv_label_set_text(ui.st_wifi, LV_SYMBOL_WIFI);
  ui.st_page = label(scr, &font_ru_14, 272, 3, 42, C_MUTED, LV_TEXT_ALIGN_RIGHT);
  hline(scr, 0, kBarH, 320);
}

// Left column text block is shifted down to sit mid-column; the warning
// gets the strip left under it.
constexpr lv_coord_t kMainDY = 12;

void build_main_page(lv_obj_t *p) {
  // Left column (0..157): SOC, mode, ETA, light, warning
  {
    // 72px digits + "%" in 28px, bottoms aligned to the shared baseline.
    // Digit ink spans label y+19..y+70 (line 89, base 19, cap 51), so y=-11
    // puts it at 8..59: 8 px above, and 8 px down to the mode text's cap top.
    lv_obj_t *row = box(p, 0, -11 + kMainDY, 158, 89);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(row, 2, 0);
    ui.soc = lv_label_create(row);
    lv_obj_set_style_text_font(ui.soc, &font_soc_72, 0);
    lv_obj_set_style_text_color(ui.soc, C_TEXT, 0);
    lv_label_set_text(ui.soc, "--");
    ui.soc_pct = lv_label_create(row);
    lv_obj_set_style_text_font(ui.soc_pct, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(ui.soc_pct, C_MUTED, 0);
    lv_obj_set_style_pad_bottom(ui.soc_pct, 19 - 5, 0);  // 72px base_line - 28px base_line
    lv_label_set_text(ui.soc_pct, "%");
  }

  ui.mode = label(p, &font_ru_20, 4, 63 + kMainDY, 150, C_TEXT, LV_TEXT_ALIGN_CENTER);
  ui.eta = label(p, &font_ru_14, 4, 88 + kMainDY, 150, C_MUTED, LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(ui.eta, "");
  ui.energy = label(p, &font_ru_14, 4, 106 + kMainDY, 150, C_MUTED, LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(ui.energy, "");
  ui.light = label(p, &font_ru_14, 4, 124 + kMainDY, 150, C_MUTED, LV_TEXT_ALIGN_CENTER);
  ui.solar = label(p, &font_ru_14, 4, 142 + kMainDY, 150, C_MUTED, LV_TEXT_ALIGN_CENTER);
  lv_label_set_text(ui.solar, "");

  // Warning in the strip under the solar line (ends at 159 + kMainDY):
  // 14 px text at 16 px pitch, one line per pack.
  constexpr lv_coord_t kWarnY = 159 + kMainDY + 4;
  constexpr lv_coord_t kWarnH = kPageH - kWarnY;
  ui.warn_box = box(p, 4, kWarnY, 152, kWarnH);
  ui.warn_icon = label(ui.warn_box, &lv_font_montserrat_16, 0, 0, 18, C_BAD);
  lv_label_set_text(ui.warn_icon, LV_SYMBOL_WARNING);
  ui.warn_text = label(ui.warn_box, &font_ru_14, 20, 0, 132, C_BAD);
  lv_obj_set_style_text_line_space(ui.warn_text, -1, 0);
  lv_obj_set_height(ui.warn_text, kWarnH);
  lv_label_set_long_mode(ui.warn_text, LV_LABEL_LONG_DOT);
  lv_obj_add_flag(ui.warn_box, LV_OBJ_FLAG_HIDDEN);

  vline(p, 158, 4, kPageH - 8);

  // Right column: current / voltage / power
  ui.cur = metric(p, tr("Current", "Ток"), tr("A", "А"), 168, 4, 148);
  // Clock: top-right corner of the right column, on the "Current" title line.
  ui.clock = label(p, &lv_font_montserrat_20, 230, 0, 86, C_MUTED, LV_TEXT_ALIGN_RIGHT);
  lv_label_set_text(ui.clock, "");
  ui.volt = metric(p, tr("Voltage", "Напряжение"), tr("V", "В"), 168, 74, 148);
  ui.pwr = metric(p, tr("Power", "Мощность"), tr("W", "Вт"), 168, 144, 148);
}

void build_pack_page(lv_obj_t *p, PackPage &pp) {
  pp.root = p;
  pp.header = label(p, &font_ru_16, 8, 2, 304);
  lv_label_set_recolor(pp.header, true);
  hline(p, 6, 23, 308);
  pp.grid = box(p, 0, 24, 320, kPageH - 24);
  lv_obj_t *g = pp.grid;
  pp.soh = cell(g, tr("Health / cycles", "Здоровье / циклы"), 0, 0);
  pp.energy = cell(g, tr("Energy left", "Запас энергии"), 1, 0);
  pp.cap = cell(g, tr("Capacity", "Емкость"), 0, 1);
  pp.temp = cell(g, tr("Temp T1/T2/MOS", "Темп. T1/T2/MOS"), 1, 1);
  pp.cells = cell(g, tr("Cell voltage", "Напряжение ячеек"), 0, 2);
  pp.dv = cell(g, tr("Cell spread", "Ячейки мин-макс"), 1, 2);
  pp.mos = cell(g, tr("Charge / discharge", "Заряд / разряд"), 0, 3);
  lv_label_set_recolor(pp.mos, true);
  pp.alarm = cell(g, tr("Protection", "Защита"), 1, 3);
  pp.ble = cell(g, tr("BLE link", "Связь BLE"), 0, 4);
  pp.wifi = cell(g, "Wi-Fi", 1, 4);
}

void build_mppt_page(lv_obj_t *p, MpptPage &mp) {
  mp.header = label(p, &font_ru_16, 8, 2, 304);
  hline(p, 6, 23, 308);
  mp.grid = box(p, 0, 24, 320, kPageH - 24);
  lv_obj_t *g = mp.grid;
  mp.pv = cell(g, tr("Panels", "Панели"), 0, 0);
  mp.bat = cell(g, tr("To battery", "Заряд АКБ"), 1, 0);
  mp.stage = cell(g, tr("Stage", "Стадия"), 0, 1);
  mp.state = cell(g, tr("Status", "Состояние"), 1, 1);
  mp.temp = cell(g, tr("Temp batt / MPPT", "Темп. АКБ / MPPT"), 0, 2);
  mp.age = cell(g, tr("Updated", "Обновлено"), 1, 2);
  mp.ble = cell(g, tr("BLE link", "Связь BLE"), 0, 3);
}

// ---- formatting ----------------------------------------------------------

struct LinkView {
  const char *text;
  lv_color_t color;
};

LinkView link_view(const BatteryState &s, uint32_t now) {
  if (s.connected) {
    if (s.is_fresh(now)) return {tr("Online", "Онлайн"), C_OK};
    if (s.has_data) return {tr("Stale", "Устарело"), C_WARN};
    return {tr("Reading...", "Чтение..."), C_WARN};
  }
  if (strcmp(s.ble_phase, "CONNECT") == 0) return {tr("Connect...", "Подключ..."), C_WARN};
  if (strcmp(s.ble_phase, "WAIT") == 0 || strcmp(s.ble_phase, "SEEN") == 0)
    return {tr("Waiting", "Ожидание"), C_WARN};
  if (s.ble_scanning) return {tr("Scanning...", "Поиск..."), C_WARN};
  if (s.has_data) return {tr("Offline", "Офлайн"), C_BAD};
  return {tr("No link", "Нет связи"), C_BAD};
}

void fmt_duration(char *buf, size_t n, const char *prefix, float hours) {
  if (hours < 0) {
    buf[0] = '\0';
    return;
  }
  const int total_m = (int)lroundf(hours * 60.0f);
  const int h = total_m / 60;
  const int m = total_m % 60;
  if (h > 99) snprintf(buf, n, tr("%s >99h", "%s >99ч"), prefix);
  else snprintf(buf, n, tr("%s %dh %02dm", "%s %dч %02dм"), prefix, h, m);
}

// "-0.0" looks odd on a display.
float nz(float v, float eps) { return fabsf(v) < eps ? 0.0f : v; }

lv_color_t dv_color(DvGrade g) {
  switch (g) {
    case DvGrade::GREEN: return C_OK;
    case DvGrade::YELLOW: return C_WARN;
    case DvGrade::RED: return C_BAD;
    default: return C_MUTED;
  }
}

lv_color_t cur_color(float a, float warn, float bad) {
  a = fabsf(a);
  return a > bad ? C_BAD : (a > warn ? C_YEL : C_TEXT);
}

uint32_t hex(lv_color_t c) { return lv_color_to32(c) & 0xFFFFFF; }

bool clock_synced() { return time(nullptr) > 1700000000; }

// net_a: bank current minus what the MPPT is pushing into it right now, i.e.
// the grid/inverter side only. Solar charging never hides an outage, and
// solar covering the load never looks like "light is back".
void track_light(const SystemSummary &sum, float net_a, uint32_t now) {
  if (!ui.saved_loaded) {
    ui.saved_loaded = true;
    ui.saved_t = persist_load_outage();
  }
  if (sum.fresh == 0) return;
  if (net_a >= -DISCHARGE_ETA_MIN_A) {
    if (ui.calm_ms == 0) ui.calm_ms = now ? now : 1;
    // BMS and MPPT are sampled at different moments; a cloud edge can make a
    // single sample look "not discharging". Close an episode only when calm.
    if (ui.out_episode && (int32_t)(now - ui.calm_ms) < (int32_t)LIGHT_END_MS) return;
    ui.light_seen = true;
    ui.out_episode = false;
    ui.out_confirmed = false;
    ui.out_t = 0;
    if (ui.saved_t != 0) {
      ui.saved_t = 0;
      persist_save_outage(0);
    }
    return;
  }
  ui.calm_ms = 0;
  if (!ui.out_episode) {
    ui.out_episode = true;
    ui.out_known = ui.light_seen;  // booted mid-discharge → start unknown
    ui.out_ms = now;
    // Rebooted during an outage: take its start from flash.
    if (!ui.light_seen && ui.saved_t != 0) ui.out_t = ui.saved_t;
  }
  if (-net_a > LIGHT_OUTAGE_MIN_A && ((int32_t)(now - ui.out_ms) >= (int32_t)LIGHT_START_MS || !ui.light_seen))
    ui.out_confirmed = true;
  if (ui.out_confirmed && ui.out_known && ui.out_t == 0 && clock_synced())
    ui.out_t = time(nullptr) - (time_t)((now - ui.out_ms) / 1000);
  if (ui.out_confirmed && ui.out_t != 0 && ui.out_t != ui.saved_t) {
    ui.saved_t = ui.out_t;
    persist_save_outage(ui.out_t);
  }
}

// Bank current minus what the solar charger feeds in = grid/inverter side.
float grid_side_a(const SystemSummary &sum, const MpptState &m, uint32_t now) {
  return sum.current_a - (m.is_fresh(now) ? m.d.bat_a : 0.0f);
}

void fmt_energy(char *buf, size_t n, float wh) {
  if (wh >= 1000.0f) snprintf(buf, n, tr("%.2f kWh", "%.2f кВтч"), wh / 1000.0f);
  else snprintf(buf, n, tr("%.0f Wh", "%.0f Втч"), wh);
}

void refresh_light(uint32_t now) {
  char buf[40];
  lv_color_t c = C_MUTED;
  if (!ui.out_confirmed) {
    if (ui.light_seen) {
      snprintf(buf, sizeof(buf), tr("Grid on", "Свет есть"));
      c = C_OK;
    } else {
      snprintf(buf, sizeof(buf), tr("Grid: --", "Свет: --"));
    }
  } else if (ui.out_t != 0 && clock_synced()) {
    const time_t t_now = time(nullptr);
    const time_t t = ui.out_t;
    struct tm lt, ln;
    localtime_r(&t, &lt);
    localtime_r(&t_now, &ln);
    if (lt.tm_yday == ln.tm_yday && lt.tm_year == ln.tm_year)
      snprintf(buf, sizeof(buf), tr("Grid off since %02d:%02d", "Свет был в %02d:%02d"), lt.tm_hour, lt.tm_min);
    else
      snprintf(buf, sizeof(buf), tr("Grid off %02d.%02d %02d:%02d", "Свет был %02d.%02d %02d:%02d"), lt.tm_mday, lt.tm_mon + 1,
               lt.tm_hour, lt.tm_min);
  } else if (!ui.out_known) {
    snprintf(buf, sizeof(buf), tr("Grid off since --:--", "Свет был в --:--"));
  } else {
    // No NTP yet: relative time.
    const uint32_t m = (now - ui.out_ms) / 60000;
    if (m < 60) snprintf(buf, sizeof(buf), tr("Grid off %lu min", "Света нет %lu мин"), (unsigned long)m);
    else snprintf(buf, sizeof(buf), tr("Grid off %luh %02lum", "Света нет %luч %02luм"), (unsigned long)(m / 60),
                  (unsigned long)(m % 60));
  }
  set_text(ui.light, buf);
  set_color(ui.light, c);
}

// ---- refresh -------------------------------------------------------------

LinkView mppt_view(const MpptState &m, uint32_t now) {
  if (m.connected) {
    if (m.is_fresh(now)) return {tr("Online", "Онлайн"), m.d.fault() ? C_BAD : C_OK};
    return {m.has_data ? tr("Stale", "Устарело") : tr("Reading...", "Чтение..."), C_WARN};
  }
  if (strcmp(m.phase, "CONNECT") == 0) return {tr("Connect...", "Подключ..."), C_WARN};
  if (strcmp(m.phase, "WAIT") == 0 || strcmp(m.phase, "BOOT") == 0) return {tr("Waiting", "Ожидание"), C_WARN};
  return {m.has_data ? tr("Offline", "Офлайн") : tr("No link", "Нет связи"), C_BAD};
}

void refresh_status_bar(const BatteryState *packs, const MpptState &m, uint32_t now) {
  char buf[32];
  for (uint8_t i = 0; i < JK_BMS_COUNT; i++) {
    const LinkView v = link_view(packs[i], now);
    snprintf(buf, sizeof(buf), "%u: %s", i + 1, v.text);
    set_text(ui.st_pack[i], buf);
    set_color(ui.st_pack[i], v.color);
  }
  if (kHasMppt) sun_set_color(ui.st_pv, mppt_view(m, now).color);
  set_color(ui.st_wifi, http_metrics_wifi_ok() ? C_OK : C_BAD);
  snprintf(buf, sizeof(buf), "%u/%u", ui.page + 1, kPages);
  set_text(ui.st_page, buf);
}

void refresh_solar(const MpptState &m, uint32_t now) {
  char buf[48];
  if (!m.is_fresh(now)) {
    set_text(ui.solar, m.has_data || m.connected ? tr("Solar: --", "Солнце: --") : "");
    set_color(ui.solar, C_MUTED);
    return;
  }
  if (m.d.pv_w < 1.0f) snprintf(buf, sizeof(buf), tr("Solar: none", "Солнце: нет"));
  else snprintf(buf, sizeof(buf), tr("Solar %.0f W", "Солнце %.0f Вт"), m.d.pv_w);
  set_text(ui.solar, buf);
  set_color(ui.solar, m.d.fault() ? C_BAD : (m.d.pv_w >= 1.0f ? C_YEL : C_MUTED));
}

void refresh_main(const BatteryState *packs, const SystemSummary &sum, const MpptState &m,
                  uint32_t now) {
  char buf[160];

  if (sum.fresh == 0) {
    set_text(ui.soc, "--");
    set_color(ui.cur, C_TEXT);
    set_text(ui.mode, tr("No data", "Нет данных"));
    set_color(ui.mode, C_MUTED);
    set_text(ui.eta, "");
    set_text(ui.energy, "");
    set_text(ui.cur, "--");
    set_text(ui.volt, "--");
    set_text(ui.pwr, "--");
  } else {
    snprintf(buf, sizeof(buf), "%.0f", sum.soc_pct);
    set_text(ui.soc, buf);

    switch (sum.mode) {
      case PackMode::CHARGE:
        set_text(ui.mode, tr("Charging", "Заряд"));
        set_color(ui.mode, C_OK);
        fmt_duration(buf, sizeof(buf), tr("full in", "до полного"), sum.eta_h);
        break;
      case PackMode::DISCHARGE:
        set_text(ui.mode, tr("Discharging", "Разряд"));
        set_color(ui.mode, C_WARN);
        fmt_duration(buf, sizeof(buf), tr("empty in", "осталось"), sum.eta_h);
        break;
      default:
        set_text(ui.mode, tr("Idle", "Простой"));
        set_color(ui.mode, C_TEXT);
        buf[0] = '\0';
        break;
    }
    set_text(ui.eta, buf);
    {
      char e[24];
      snprintf(e, sizeof(e), tr("%.0f Wh", "%.0f Втч"), sum.energy_wh);
      if (sum.fresh < JK_BMS_COUNT)  // totals cover only the packs that answer
        snprintf(buf, sizeof(buf), tr("stored %s (%u/%u)", "запас %s (%u/%u)"), e, sum.fresh, (unsigned)JK_BMS_COUNT);
      else snprintf(buf, sizeof(buf), tr("stored %s", "запас %s"), e);
      set_text(ui.energy, buf);
    }

    snprintf(buf, sizeof(buf), "%+.1f", nz(sum.current_a, 0.05f));
    set_text(ui.cur, buf);
    set_color(ui.cur, cur_color(sum.current_a, TOTAL_CUR_WARN_A, TOTAL_CUR_BAD_A));
    snprintf(buf, sizeof(buf), "%.2f", sum.voltage_v);
    set_text(ui.volt, buf);
    snprintf(buf, sizeof(buf), "%+.0f", nz(sum.power_w, 0.5f));
    set_text(ui.pwr, buf);
  }

  refresh_light(now);
  refresh_solar(m, now);

  if (clock_synced()) {
    const time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
    set_text(ui.clock, buf);
  } else {
    set_text(ui.clock, "");  // no NTP yet — show nothing rather than a wrong time
  }

  // Warning: any issue on any pack
  char line[48];
  buf[0] = '\0';
  bool severe = false;
  for (uint8_t i = 0; i < JK_BMS_COUNT; i++) {
    const uint16_t is = pack_issues(packs[i], now);
    if (!is) continue;
    severe |= (is & (ISSUE_BMS_ALARM | ISSUE_LOST | ISSUE_TEMP_HIGH)) != 0;
    pack_issue_text(packs[i], is, line, sizeof(line));
    const size_t l = strlen(buf);
    snprintf(buf + l, sizeof(buf) - l, tr("%sPack%u: %s", "%sАКБ%u: %s"), l ? "\n" : "", i + 1, line);
  }
  set_hidden(ui.warn_box, buf[0] == '\0');
  if (buf[0]) {
    set_text(ui.warn_text, buf);
    set_color(ui.warn_icon, severe ? C_BAD : C_WARN);
    set_color(ui.warn_text, severe ? C_BAD : C_WARN);
  }
}

void refresh_pack(PackPage &pp, uint8_t idx, const BatteryState &s, uint32_t now) {
  char buf[96];
  const PackData &d = s.d;
  const bool fresh = s.is_fresh(now);

  if (!s.has_data) {
    snprintf(buf, sizeof(buf), tr("Pack %u: no data", "АКБ %u: нет данных"), idx + 1);
  } else if (!fresh) {
    const uint32_t age_s = (now - s.last_update_ms) / 1000;
    if (age_s < 120) snprintf(buf, sizeof(buf), tr("Pack %u: stale (%lu s)", "АКБ %u: устарело (%lu с)"), idx + 1, (unsigned long)age_s);
    else snprintf(buf, sizeof(buf), tr("Pack %u: stale (%lu min)", "АКБ %u: устарело (%lu мин)"), idx + 1, (unsigned long)(age_s / 60));
  } else {
    const lv_color_t cc = cur_color(d.current_a, PACK_CUR_WARN_A, PACK_CUR_BAD_A);
    char cur[24];
    if (cc.full == C_TEXT.full) snprintf(cur, sizeof(cur), tr("%+.1f A", "%+.1f А"), nz(d.current_a, 0.05f));
    else snprintf(cur, sizeof(cur), tr("#%06lX %+.1f A#", "#%06lX %+.1f А#"), (unsigned long)hex(cc), nz(d.current_a, 0.05f));
    snprintf(buf, sizeof(buf), tr("Pack %u  %.0f%%  %.2f V  %s  %+.0f W", "АКБ %u  %.0f%%  %.2f В  %s  %+.0f Вт"), idx + 1, d.soc_pct,
             d.voltage_v, cur, nz(d.power_w, 0.5f));
  }
  set_text(pp.header, buf);
  set_color(pp.header, fresh ? C_TEXT : (s.has_data ? C_WARN : C_MUTED));
  set_opa(pp.grid, fresh || !s.has_data ? LV_OPA_COVER : LV_OPA_50);

  // Link info is useful even without telemetry.
  if (s.connected || s.rssi_dbm != 0) {
    const int r = s.rssi_dbm;
    if (r == 0) snprintf(buf, sizeof(buf), tr("up", "есть"));
    else snprintf(buf, sizeof(buf), tr("%d dBm%s", "%d дБм%s"), r, s.connected ? "" : tr(", down", ", нет"));
    set_text(pp.ble, buf);
    set_color(pp.ble, !s.connected ? C_BAD : (r != 0 && r < -85) ? C_BAD : (r != 0 && r < -75) ? C_WARN : C_OK);
  } else {
    set_text(pp.ble, s.ble_scanning ? tr("scanning", "поиск") : tr("none", "нет"));
    set_color(pp.ble, C_BAD);
  }
  {
    char ip[20] = "";
    http_metrics_ip(ip, sizeof(ip));
    set_text(pp.wifi, ip[0] ? ip : tr("off", "нет"));
    set_color(pp.wifi, ip[0] ? C_TEXT : C_BAD);
  }

  if (!s.has_data) {
    lv_obj_t *vals[] = {pp.soh, pp.energy, pp.cap, pp.temp, pp.cells, pp.dv, pp.mos, pp.alarm};
    for (lv_obj_t *v : vals) {
      set_text(v, "--");
      set_color(v, C_TEXT);
    }
    return;
  }

  snprintf(buf, sizeof(buf), "%.0f %% / %lu", d.soh_pct, (unsigned long)d.cycle_count);
  set_text(pp.soh, buf);
  set_color(pp.soh, d.soh_pct >= 80 ? C_TEXT : (d.soh_pct >= 60 ? C_WARN : C_BAD));

  fmt_energy(buf, sizeof(buf), s.remaining_ah() * d.voltage_v);
  set_text(pp.energy, buf);

  snprintf(buf, sizeof(buf), tr("%.1f / %.0f Ah", "%.1f / %.0f Ач"), s.remaining_ah(), s.nominal_ah());
  set_text(pp.cap, buf);

  {
    char t[3][8];
    const float tv[3] = {d.temp1_c, d.temp2_c, d.temp_mos_c};
    bool hot = false;
    for (int i = 0; i < 3; i++) {
      if (temp_valid(tv[i])) snprintf(t[i], sizeof(t[i]), "%.0f", tv[i]);
      else snprintf(t[i], sizeof(t[i]), "--");
    }
    hot = (temp_valid(d.temp1_c) && d.temp1_c >= TEMP_CELL_WARN_HIGH_C) ||
          (temp_valid(d.temp2_c) && d.temp2_c >= TEMP_CELL_WARN_HIGH_C) ||
          (temp_valid(d.temp_mos_c) && d.temp_mos_c >= TEMP_MOS_WARN_HIGH_C);
    snprintf(buf, sizeof(buf), "%s / %s / %s °C", t[0], t[1], t[2]);
    set_text(pp.temp, buf);
    set_color(pp.temp, hot ? C_BAD : C_TEXT);
  }

  if (d.cell_count) snprintf(buf, sizeof(buf), tr("%.3f-%.3f V", "%.3f-%.3f В"), d.cell_min_v, d.cell_max_v);
  else snprintf(buf, sizeof(buf), "--");
  set_text(pp.cells, buf);

  snprintf(buf, sizeof(buf), tr("%.0f mV%s", "%.0f мВ%s"), d.cell_delta_v * 1000.0f, d.balancing ? tr(", balancing", ", баланс") : "");
  set_text(pp.dv, buf);
  const uint16_t issues = pack_issues(s, now);
  set_color(pp.dv, (issues & ISSUE_IMBALANCE) ? C_BAD : dv_color(dv_grade(d)));

  snprintf(buf, sizeof(buf), "#%06lX %s# / #%06lX %s#",
           (unsigned long)hex(d.charging_mos ? C_OK : C_BAD), d.charging_mos ? tr("on", "вкл") : tr("off", "выкл"),
           (unsigned long)hex(d.discharging_mos ? C_OK : C_BAD), d.discharging_mos ? tr("on", "вкл") : tr("off", "выкл"));
  set_text(pp.mos, buf);

  if (jk_alarm_active(d.errors_bitmask)) {
    jk_alarm_list(d.errors_bitmask, buf, sizeof(buf));
    set_text(pp.alarm, buf);
    set_color(pp.alarm, C_BAD);
  } else {
    set_text(pp.alarm, tr("OK", "Норма"));
    set_color(pp.alarm, C_OK);
  }
}

const char *stage_text(uint8_t st) {
  switch (st) {
    case 1: return tr("Float", "Поддержка (float)");
    case 2: return tr("Boost", "Основной (boost)");
    case 3: return tr("Equalize", "Выравнивание");
    default: return tr("Not charging", "Не заряжает");
  }
}

void refresh_mppt(MpptPage &mp, const MpptState &m, uint32_t now) {
  char buf[96];
  const bool fresh = m.is_fresh(now);
  const MpptData &d = m.d;
  if (!m.has_data) snprintf(buf, sizeof(buf), tr("Solar controller: no data", "Солнечный контроллер: нет данных"));
  else snprintf(buf, sizeof(buf), tr("Solar  %.0f W  to battery %.0f W", "Солнце  %.0f Вт  в АКБ %.0f Вт"), d.pv_w, d.bat_w);
  set_text(mp.header, buf);
  set_color(mp.header, fresh ? C_TEXT : (m.has_data ? C_WARN : C_MUTED));
  set_opa(mp.grid, fresh || !m.has_data ? LV_OPA_COVER : LV_OPA_50);

  if (m.connected || m.rssi_dbm != 0) {
    const int r = m.rssi_dbm;
    if (r == 0) snprintf(buf, sizeof(buf), tr("up", "есть"));
    else snprintf(buf, sizeof(buf), tr("%d dBm%s", "%d дБм%s"), r, m.connected ? "" : tr(", down", ", нет"));
    set_text(mp.ble, buf);
    set_color(mp.ble, !m.connected || (r != 0 && r < -85) ? C_BAD : (r != 0 && r < -75) ? C_WARN : C_OK);
  } else {
    set_text(mp.ble, mppt_view(m, now).text);
    set_color(mp.ble, mppt_view(m, now).color);
  }

  if (!m.has_data) {
    lv_obj_t *v[] = {mp.pv, mp.bat, mp.stage, mp.temp, mp.state, mp.age};
    for (lv_obj_t *o : v) {
      set_text(o, "--");
      set_color(o, C_TEXT);
    }
    return;
  }

  snprintf(buf, sizeof(buf), tr("%.1f V  %.2f A", "%.1f В  %.2f А"), d.pv_v, d.pv_a);
  set_text(mp.pv, buf);
  snprintf(buf, sizeof(buf), tr("%.2f V  %.1f A", "%.2f В  %.1f А"), d.bat_v, d.bat_a);
  set_text(mp.bat, buf);

  if (d.has_status) {
    set_text(mp.stage, stage_text(d.stage()));
    set_color(mp.stage, d.stage() ? C_OK : C_MUTED);
  } else set_text(mp.stage, "--");

  if (d.has_temps) {
    snprintf(buf, sizeof(buf), "%.0f / %.0f °C", d.temp_bat_c, d.temp_dev_c);
    set_text(mp.temp, buf);
    set_color(mp.temp, d.temp_dev_c >= 70 ? C_BAD : (d.temp_dev_c >= 55 ? C_WARN : C_TEXT));
  } else set_text(mp.temp, "--");

  if (!d.has_status) {
    set_text(mp.state, "--");
    set_color(mp.state, C_TEXT);
  } else if (d.chg_status & 0x2) {
    set_text(mp.state, tr("Charge fault", "Авария заряда"));
    set_color(mp.state, C_BAD);
  } else {
    static const char *bs[] = {tr("OK", "Норма"), tr("Batt overvolt", "АКБ: перенапряж."), tr("Batt undervolt", "АКБ: низкое напр."), tr("Batt disconnected", "АКБ: отключена"),
                               tr("Batt fault", "АКБ: ошибка")};
    const uint8_t b = d.bat_status & 0x0F;
    set_text(mp.state, b <= 4 ? bs[b] : tr("Batt: ?", "АКБ: ?"));
    set_color(mp.state, b == 0 ? C_OK : C_BAD);
  }

  const uint32_t age = (now - m.last_update_ms) / 1000;
  if (age < 120) snprintf(buf, sizeof(buf), tr("%lu s ago", "%lu с назад"), (unsigned long)age);
  else snprintf(buf, sizeof(buf), tr("%lu min ago", "%lu мин назад"), (unsigned long)(age / 60));
  set_text(mp.age, buf);
  set_color(mp.age, fresh ? C_TEXT : C_WARN);
}

void refresh() {
  BatteryState packs[JK_BMS_COUNT];
  battery_state_snapshot(packs);
  const MpptState m = mppt_state_snapshot();
  const uint32_t now = millis();

  const SystemSummary sum = summarize(packs, JK_BMS_COUNT, now);
  track_light(sum, grid_side_a(sum, m, now), now);  // on every tick, whatever page is shown

  refresh_status_bar(packs, m, now);
  if (ui.page == 0) refresh_main(packs, sum, m, now);
  else if (ui.page == kMpptPage) refresh_mppt(ui.mp, m, now);
  else refresh_pack(ui.pp[ui.page - 1], ui.page - 1, packs[ui.page - 1], now);
}

void show_page(uint8_t page) {
  page %= kPages;
  if (page == ui.page) return;
  set_hidden(ui.pages[ui.page], true);
  ui.page = page;
  set_hidden(ui.pages[ui.page], false);
  refresh();  // fill the newly shown page right away
}

void on_tap(lv_event_t *) { show_page(ui.page + 1); }

void on_timer(lv_timer_t *) {
  if (ui.page != 0 && lv_disp_get_inactive_time(nullptr) >= UI_IDLE_HOME_MS) {
    show_page(0);
    return;
  }
  refresh();
}

}  // namespace

void ui_init() {
  lv_style_init(&st_box);
  lv_style_set_bg_opa(&st_box, LV_OPA_TRANSP);
  lv_style_set_border_width(&st_box, 0);
  lv_style_set_pad_all(&st_box, 0);
  lv_style_set_radius(&st_box, 0);

  lv_obj_t *scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, C_BG, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scr, on_tap, LV_EVENT_CLICKED, nullptr);

  build_status_bar(scr);
  for (uint8_t i = 0; i < kPages; i++) {
    ui.pages[i] = box(scr, 0, kPageY, 320, kPageH);
    if (i == 0) build_main_page(ui.pages[i]);
    else if (i == kMpptPage) build_mppt_page(ui.pages[i], ui.mp);
    else build_pack_page(ui.pages[i], ui.pp[i - 1]);
    if (i != 0) lv_obj_add_flag(ui.pages[i], LV_OBJ_FLAG_HIDDEN);
  }
  ui.page = 0;

  lv_timer_create(on_timer, UI_REFRESH_MS, nullptr);
  refresh();
}

// Screen asleep: LVGL is not run, but the outage tracker must keep going.
void ui_sleep_tick() {
  BatteryState packs[JK_BMS_COUNT];
  battery_state_snapshot(packs);
  const uint32_t now = millis();
  const SystemSummary sum = summarize(packs, JK_BMS_COUNT, now);
  track_light(sum, grid_side_a(sum, mppt_state_snapshot(), now), now);
}

void ui_wake() {
  if (ui.page != 0) show_page(0);
  else refresh();
  lv_obj_invalidate(lv_scr_act());
}

bool ui_saver_hours() {
  if (SAVER_FROM_H == SAVER_TO_H || !clock_synced()) return false;
  const time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  const int h = lt.tm_hour;
  return SAVER_FROM_H < SAVER_TO_H ? (h >= SAVER_FROM_H && h < SAVER_TO_H)
                                   : (h >= SAVER_FROM_H || h < SAVER_TO_H);
}

int ui_saver_scene() {
  if (!clock_synced()) return SAVER_NIGHT;
  const time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  static const uint8_t HOURS[4][4] = SAVER_SEASON_HOURS;
  const uint8_t *sh = HOURS[(lt.tm_mon + 1) % 12 / 3];  // Dec..Feb -> 0
  const int h = lt.tm_hour;
  // Late night may start before or after midnight.
  if (sh[3] > sh[0] ? (h >= sh[3] || h < sh[0]) : (h >= sh[3] && h < sh[0])) return SAVER_LATE;
  if (h >= sh[0] && h < sh[1]) return SAVER_MORNING;
  if (h >= sh[1] && h < sh[2]) return SAVER_DAY;
  return SAVER_NIGHT;
}

void ui_saver_hud(char *buf, unsigned n) {
  BatteryState packs[JK_BMS_COUNT];
  battery_state_snapshot(packs);
  const SystemSummary sum = summarize(packs, JK_BMS_COUNT, millis());
  const time_t t = time(nullptr);
  struct tm lt;
  localtime_r(&t, &lt);
  if (sum.fresh) snprintf(buf, n, "%02d:%02d %d%%", lt.tm_hour, lt.tm_min, (int)lroundf(sum.soc_pct));
  else snprintf(buf, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
}
