#include "ui.h"
#include "display.h"
#include "theme.h"
#include "settings.h"
#include "keyboard.h"
#include "power.h"
#include "powerlog.h"
#include "bluetti.h"
#include "logic/geom.h"

// ===== Model ================================================================
enum Screen { POWER, POWER_CHART, BT_SETTINGS };

// Which field the keyboard is currently editing (so we can store on commit).
enum EditTarget { EDIT_NONE, EDIT_BTMAC };
static EditTarget editTarget = EDIT_NONE;

// Power screen output-toggle confirm state: 0 none, 1 AC, 2 DC.
static int pwrConfirm = 0;
static bool pwrConfirmTarget = false;

static Screen current = POWER;

// Computed layout (depends on resolution / rotation).
static int16_t gearX, gearY, gearW, gearH;  // settings-gear tap rect

// Bluetti-settings list row hit-rect geometry base.
static int16_t nameRowX, nameRowY, nameRowW, nameRowH;

// inRect() is provided by logic/geom.h (pure + unit-tested).

// ===== Icons (drawn with primitives, centered on cx,cy) =====================
static void iconGear(int16_t cx, int16_t cy, uint16_t c) {
  // Eight teeth + hub.
  for (int i = 0; i < 8; i++) {
    float a = i * (PI / 4);
    int16_t x = cx + (int16_t)(cos(a) * 9);
    int16_t y = cy + (int16_t)(sin(a) * 9);
    gfx->fillRect(x - 2, y - 2, 4, 4, c);
  }
  gfx->fillCircle(cx, cy, 7, c);
  gfx->fillCircle(cx, cy, 3, COL_BAR);
}

// ===== Layout ===============================================================
static void layout() {
  const int16_t W = gfx->width(), H = gfx->height();
  const int16_t margin = 12;

  // Settings gear: no status bar to anchor it to anymore, so it sits in the
  // empty gap between the AC IN (top) and AC OUT (bottom) cards, right-hand
  // side. Matches powerCardRect's own geometry (m=10, card 140x80).
  const int16_t cardM = 10, cardW = 140, cardH = 80;
  const int16_t gapCx = W - cardM - cardW / 2;
  const int16_t gapTop = cardM + cardH, gapBot = H - cardM - cardH;
  gearW = gearH = 44;
  gearX = gapCx - gearW / 2;
  gearY = (gapTop + gapBot) / 2 - gearH / 2;

  // Bluetti-settings list rows — compact pitch to fit a top utility row
  // (history/release) plus 6 rows on the 320px-tall screen (3 toggles,
  // charge-limit, screen-timeout, pairing) with no title to spend space on.
  nameRowX = margin;
  nameRowY = 56;
  nameRowW = W - 2 * margin;
  nameRowH = 36;
}

// ===== Bluetooth link icon (shown only while offline/connecting) ===========
// No persistent status bar in this build: the SoC is already the headline
// number on the gauge (a battery icon would be redundant), and the Bluetooth
// icon is only useful while there's no live data to show yet, so it lives on
// the "Bluetti offline" screen instead of a bar every screen has to carry.
static uint8_t btAnimPhase = 0;  // toggles while connecting to the Bluetti
static void iconBluetooth(int16_t cx, int16_t cy, uint16_t c) {
  const int16_t h = 7, w = 4, q = 3;
  gfx->drawLine(cx, cy - h, cx, cy + h, c);         // spine
  gfx->drawLine(cx, cy - h, cx + w, cy - q, c);     // top tip -> upper right
  gfx->drawLine(cx, cy + h, cx + w, cy + q, c);     // bottom tip -> lower right
  gfx->drawLine(cx - w, cy - q, cx + w, cy + q, c); // cross stroke
  gfx->drawLine(cx - w, cy + q, cx + w, cy - q, c); // cross stroke
}

// NOTE: reg 152 (power.tempC) is intentionally not shown anywhere in the UI.
// It reads a plausible value at first use but only ever creeps upward over
// weeks, never dipping even overnight — behaviour consistent with a
// BMS-internal peak/record-high stat, not the live battery temperature
// (which the Bluetti app itself doesn't expose either). Don't wire it back
// in without confirming what it actually is.

// Thick rounded arc gauge: a 270-degree track (open at the bottom) with the
// lower `frac` portion filled. Drawn from overlapping dots so it has rounded
// ends without needing library arc support.
static void drawGaugeRing(int16_t cx, int16_t cy, int16_t rOuter,
                          int16_t thick, float frac, uint16_t fillCol,
                          uint16_t trackCol) {
  const float A0 = 135.0f, SWEEP = 270.0f;  // start bottom-left, sweep over top
  const float rm = rOuter - thick / 2.0f;
  const int16_t dot = thick / 2;
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;
  for (float a = 0; a <= SWEEP; a += 2.5f) {
    float th = (A0 + a) * (float)PI / 180.0f;
    int16_t px = cx + (int16_t)(rm * cosf(th));
    int16_t py = cy + (int16_t)(rm * sinf(th));
    gfx->fillCircle(px, py, dot, a <= SWEEP * frac ? fillCol : trackCol);
  }
}

// Corner card geometry: 0=DC IN (TL), 1=AC IN (TR), 2=DC OUT (BL), 3=AC OUT (BR).
static void powerCardRect(int idx, int16_t& x, int16_t& y, int16_t& w,
                          int16_t& h) {
  const int16_t W = gfx->width(), H = gfx->height(), m = 10;
  w = 140;
  h = 80;
  x = (idx == 0 || idx == 2) ? m : (W - m - w);
  y = (idx < 2) ? m : (H - m - h);  // no status bar -- top cards sit at the margin
}

// One corner readout card: short bold label + big watt value. Output cards
// (toggle 0/1) also get a state border + a bottom-left on-state dot (nothing
// when off -- the border colour already carries that) and act as toggle
// buttons.
static void drawPowerCard(int idx, const char *label, int watts,
                          uint16_t labelCol, int toggle) {
  int16_t x, y, w, h;
  powerCardRect(idx, x, y, w, h);
  gfx->fillRoundRect(x, y, w, h, 10, COL_TILE);
  if (toggle >= 0)
    gfx->drawRoundRect(x, y, w, h, 10, toggle ? COL_ON : COL_MUTED);
  drawCenteredText(label, x + w / 2, y + 18, 2, labelCol);  // larger label
  // Big watt value with a small trailing "w". FreeSans is proportional, not
  // monospace, so measure both pieces rather than guessing glyph widths.
  char v[8];
  snprintf(v, sizeof(v), "%d", watts);
  int16_t numW, numH, wW, wH;
  measureText(v, 4, numW, numH);
  measureText("w", 2, wW, wH);
  const int16_t gap = 2;
  const int16_t sx = x + w / 2 - (numW + gap + wW) / 2;
  const int16_t numTop = y + 48 - numH / 2;  // center number block on y+48
  drawText(v, sx, numTop, 4, COL_TEXT);
  drawText("w", sx + numW + gap, numTop + numH - wH, 2, COL_MUTED);
  if (toggle == 1) gfx->fillCircle(x + 14, y + h - 10, 5, COL_ON);
}

// Small phone glyph for the app-release button.
static void iconPhone(int16_t cx, int16_t cy, uint16_t c) {
  gfx->drawRoundRect(cx - 8, cy - 13, 16, 26, 3, c);
  gfx->drawRoundRect(cx - 7, cy - 12, 14, 24, 3, c);
  gfx->drawFastHLine(cx - 3, cy - 9, 6, c);  // earpiece
  gfx->fillCircle(cx, cy + 9, 1, c);         // home button
}

// Bottom-centre charge-mode button on the Power screen (the only one left
// there -- history and app-release moved to the Bluetti Settings page).
static const int16_t PWR_BTN_W = 50, PWR_BTN_H = 42, PWR_BTN_Y = 270;
static void powerChargeBtnRect(int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
  w = PWR_BTN_W; h = PWR_BTN_H; y = PWR_BTN_Y;
  x = gfx->width() / 2 - PWR_BTN_W / 2;  // centre
}

// Top-of-screen utility row on the Bluetti Settings page: history (left) and
// app-release (right). Always available regardless of connection state, so
// callers must draw/handle these before any power_valid() gate.
static const int16_t BT_BTN_Y = 8;
static void btChartBtnRect(int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
  w = PWR_BTN_W; h = PWR_BTN_H; y = BT_BTN_Y;
  x = gfx->width() / 2 - 6 - PWR_BTN_W;  // left of centre
}
static void btReleaseBtnRect(int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
  w = PWR_BTN_W; h = PWR_BTN_H; y = BT_BTN_Y;
  x = gfx->width() / 2 + 6;  // right of centre
}

// Mini bar-chart glyph for the history button.
static void iconChart(int16_t cx, int16_t cy, uint16_t c) {
  const int16_t bl = cy + 9;  // baseline
  gfx->fillRect(cx - 10, bl - 6, 4, 6, c);
  gfx->fillRect(cx - 4, bl - 10, 4, 10, c);
  gfx->fillRect(cx + 2, bl - 14, 4, 14, c);
  gfx->fillRect(cx + 8, bl - 18, 4, 18, c);
}
static void drawChartButton() {
  int16_t x, y, w, h;
  btChartBtnRect(x, y, w, h);
  gfx->fillRoundRect(x, y, w, h, 8, COL_TILE);
  gfx->drawRoundRect(x, y, w, h, 8, COL_MUTED);
  iconChart(x + w / 2, y + h / 2, ACC_WATER);
}
static void drawReleaseButton(int rem) {
  int16_t x, y, w, h;
  btReleaseBtnRect(x, y, w, h);
  gfx->fillRoundRect(x, y, w, h, 8, rem > 0 ? ACC_SETTINGS : COL_TILE);
  gfx->drawRoundRect(x, y, w, h, 8, COL_MUTED);
  iconPhone(x + w / 2, y + h / 2, rem > 0 ? COL_BG : COL_TEXT);
}

// Centered confirm modal box.
static const int16_t PWR_MODAL_W = 280, PWR_MODAL_H = 132;
static void powerModalRect(int16_t& x, int16_t& y, int16_t& w, int16_t& h) {
  w = PWR_MODAL_W;
  h = PWR_MODAL_H;
  x = (gfx->width() - w) / 2;
  y = (gfx->height() - h) / 2;
}
// Cancel / Confirm button geometry inside the modal.
static void powerConfirmRects(int16_t& cancelX, int16_t& confirmX, int16_t& y,
                              int16_t& w, int16_t& h) {
  int16_t bx, by, bw, bh;
  powerModalRect(bx, by, bw, bh);
  w = 112;
  h = 42;
  y = by + bh - h - 16;
  const int16_t gap = 16;
  cancelX = bx + bw / 2 - gap / 2 - w;
  confirmX = bx + bw / 2 + gap / 2;
}

// Charging-mode glyph: 0 Standard (bolt), 1 Silent (moon), 2 Turbo (fast-fwd),
// 4 Custom (sliders).
static void drawChargeModeIcon(int mode, int16_t cx, int16_t cy, uint16_t c) {
  if (mode == 1) {  // moon
    gfx->fillCircle(cx, cy, 8, c);
    gfx->fillCircle(cx + 4, cy - 3, 8, COL_BG);
  } else if (mode == 2) {  // turbo: double fast-forward
    gfx->fillTriangle(cx - 8, cy - 6, cx - 8, cy + 6, cx - 1, cy, c);
    gfx->fillTriangle(cx, cy - 6, cx, cy + 6, cx + 7, cy, c);
  } else if (mode == 4) {  // custom: sliders
    gfx->drawFastHLine(cx - 7, cy - 3, 14, c);
    gfx->fillRect(cx + 1, cy - 5, 4, 5, c);
    gfx->drawFastHLine(cx - 7, cy + 3, 14, c);
    gfx->fillRect(cx - 5, cy + 1, 4, 5, c);
  } else {  // standard: lightning bolt
    gfx->fillTriangle(cx + 2, cy - 8, cx - 4, cy + 1, cx + 1, cy + 1, c);
    gfx->fillTriangle(cx - 2, cy + 8, cx + 4, cy - 1, cx - 1, cy - 1, c);
  }
}

// Bottom-centre charge-mode button (icon-only; tap cycles the three main modes).
// Border/icon go green while the unit is actively charging.
static void drawChargeModeButton() {
  int16_t x, y, w, h;
  powerChargeBtnRect(x, y, w, h);
  uint16_t c = power.charging ? COL_ON : COL_TEXT;
  gfx->fillRoundRect(x, y, w, h, 8, COL_TILE);
  gfx->drawRoundRect(x, y, w, h, 8, power.charging ? COL_ON : COL_MUTED);
  drawChargeModeIcon(power.chargeMode, x + w / 2, y + h / 2, c);
}

// Power monitor: a central battery ring gauge (SoC % + remaining time) framed
// by four corner cards — DC/AC input (top) and DC/AC output (bottom), plus
// AC/DC output toggles. Full-height (no status bar); the settings gear sits
// in the gap between AC IN and AC OUT. This is the home screen.
static void drawPowerScreen() {
  const int16_t W = gfx->width(), H = gfx->height();
  gfx->fillScreen(COL_BG);
  // Settings gear -- drawn unconditionally (before the early returns below)
  // so it's reachable regardless of connection state.
  iconGear(gearX + gearW / 2, gearY + gearH / 2, COL_MUTED);

  const uint16_t IN_COL = RGB565(0, 176, 255);    // inputs: cyan/blue
  const uint16_t OUT_COL = RGB565(255, 150, 40);  // outputs: amber
  const uint16_t TRACK = RGB565(38, 42, 52);      // unfilled gauge track

  // App mode: link released so the phone app can connect. No dedicated
  // button anymore (release/resume both live on the Settings page) -- any
  // tap on this screen resumes early, see ui_handle_touch.
  int rem = (int)bluetti_release_remaining();
  if (rem > 0) {
    drawCenteredText("Released for app", W / 2, H / 2 - 24, 3, ACC_SETTINGS);
    char t[32];
    snprintf(t, sizeof(t), "Reconnecting in %ds", rem);
    drawCenteredText(t, W / 2, H / 2 + 14, 2, COL_MUTED);
    drawCenteredText("(tap to resume now)", W / 2, H / 2 + 44, 1, COL_MUTED);
    return;
  }

  if (!power_valid()) {
    // Bluetooth icon: flashes while actively connecting, muted otherwise --
    // the only place this app shows link state, since it's only useful
    // before there's live data to show instead.
    BluettiConn bt = bluetti_state();
    uint16_t btCol = (bt == BTC_CONNECTING)
                         ? ((btAnimPhase & 1) ? COL_OFF : ACC_WEATHER)
                         : COL_MUTED;
    iconBluetooth(W / 2, H / 2 - 66, btCol);
    drawCenteredText("Bluetti offline", W / 2, H / 2 - 30, 3, COL_WARN);
    drawCenteredText(strlen(settings.bluettiMac)
                         ? "Connecting to the Bluetti..."
                         : "Searching for the Bluetti...",
                     W / 2, H / 2 + 6, 2, COL_MUTED);
    return;
  }

  // Four corner cards; the two OUTPUT cards double as on/off toggles.
  drawPowerCard(0, "DC IN", power.dcInW, IN_COL, -1);
  drawPowerCard(1, "AC IN", power.acInW, IN_COL, -1);
  drawPowerCard(2, "DC OUT", power.dcOutW, OUT_COL, power.dcOn ? 1 : 0);
  drawPowerCard(3, "AC OUT", power.acOutW, OUT_COL, power.acOn ? 1 : 0);

  // Centre ring gauge.
  const int16_t cx = W / 2, cy = H / 2, rOuter = 74, thick = 12;
  uint16_t ringCol = power.soc <= 15 ? COL_WARN : IN_COL;
  drawGaugeRing(cx, cy, rOuter, thick, power.soc / 100.0f, ringCol, TRACK);

  // Big SoC number with a small superscript %. FreeSans is proportional, not
  // monospace, so measure both pieces rather than guessing glyph widths.
  char num[6];
  snprintf(num, sizeof(num), "%d", power.soc);
  int16_t numW, numH, pctW, pctH;
  measureText(num, 5, numW, numH);
  measureText("%", 2, pctW, pctH);
  const int16_t gap = 6;
  const int16_t total = numW + gap + pctW;
  const int16_t x0 = cx - total / 2, numTop = cy - numH / 2;
  drawText(num, x0, numTop, 5, ringCol);
  // Vertically center the % against the number rather than top-aligning it --
  // the bold digits are much taller than the old built-in font, so top-align
  // left it floating up near the ring stroke instead of reading as a suffix.
  drawText("%", x0 + numW + gap, numTop + (numH - pctH) / 2, 2, ringCol);

  // Remaining time (device estimate, reg 104) below the gauge — green when
  // charging, white when discharging.
  if (power.ttfMin > 0 && power.ttfMin < 100 * 60) {
    char tt[16];
    snprintf(tt, sizeof(tt), "%dh %02dm", power.ttfMin / 60, power.ttfMin % 60);
    drawCenteredText(tt, cx, 250, 2, power.charging ? COL_ON : COL_TEXT);
  }

  // Charge-mode button, or a centered confirm modal when a toggle is pending.
  if (pwrConfirm == 0) {
    drawChargeModeButton();
  } else {
    int16_t mx, my, mw, mh;
    powerModalRect(mx, my, mw, mh);
    gfx->fillRoundRect(mx, my, mw, mh, 12, COL_TILE_DN);
    gfx->drawRoundRect(mx, my, mw, mh, 12, COL_MUTED);
    char q[20];
    snprintf(q, sizeof(q), "Turn %s %s?", pwrConfirm == 1 ? "AC" : "DC",
             pwrConfirmTarget ? "ON" : "OFF");
    drawCenteredText(q, mx + mw / 2, my + 38, 2, COL_TEXT);
    int16_t cancelX, confirmX, by, bw, bh;
    powerConfirmRects(cancelX, confirmX, by, bw, bh);
    gfx->fillRoundRect(cancelX, by, bw, bh, 8, COL_OFF);
    drawCenteredText("Cancel", cancelX + bw / 2, by + bh / 2, 2, COL_BG);
    gfx->fillRoundRect(confirmX, by, bw, bh, 8,
                       pwrConfirmTarget ? COL_ON : COL_WARN);
    drawCenteredText("Confirm", confirmX + bw / 2, by + bh / 2, 2, COL_BG);
  }
}

// ===== Power history chart ==================================================
// Four flows overlaid: Solar (DC in), AC In, DC Out, AC Out. Legend chips double
// as show/hide filters; 1h/6h/24h buttons pick the X span. Fed by powerlog.
// Series 0..3 are watts (share the auto-scaled left axis); series 4 (SoC) is a
// percentage on its own fixed 0..100 right scale (see SOC_SERIES handling).
#define NSER 5
#define SOC_SERIES 4
static const uint16_t SERIES_COL[NSER] = {COL_ON, ACC_WEATHER, ACC_LIGHTS,
                                          ACC_WATER, ACC_SETTINGS};
static const char *SERIES_LBL[NSER] = {"Solar", "AC In", "DC Out", "AC Out",
                                       "SoC"};
static const char *CHART_WIN_LBL[3] = {"1h", "6h", "24h"};

// Per-column downsample cache (one entry per plot pixel-column). Filled once per
// redraw so auto-scale + drawing are O(plot width), not O(samples buffered).
#define CHART_PLOTW 430  // = PX1 - PX0 in drawChartScreen
static int16_t g_col[CHART_PLOTW][NSER];

static int seriesNow(int k) {
  switch (k) {
    case 0: return power.dcInW;
    case 1: return power.acInW;
    case 2: return power.dcOutW;
    case 3: return power.acOutW;
    default: return power.soc;
  }
}
static int chartWindowSamples() {
  static const int W[3] = {240, 1440, 5760};  // 1h/6h/24h at one sample / 15 s
  int w = settings.chartWindow > 2 ? 0 : settings.chartWindow;
  return W[w];
}

// Legend chips along the bottom (also the per-series filters). Five across.
static void chartLegendRect(int i, int16_t &x, int16_t &y, int16_t &w,
                            int16_t &h) {
  w = 91; h = 52; y = 260;
  x = 4 + i * 95;
}
// Window-span buttons, top-right.
static void chartWindowRect(int i, int16_t &x, int16_t &y, int16_t &w,
                            int16_t &h) {
  w = 44; h = 24; y = 8;
  x = 330 + i * 50;
}

static void drawChartLegend() {
  for (int i = 0; i < NSER; i++) {
    int16_t x, y, w, h;
    chartLegendRect(i, x, y, w, h);
    bool on = settings.chartSeriesMask & (1 << i);
    gfx->fillRoundRect(x, y, w, h, 8, COL_TILE);
    gfx->drawRoundRect(x, y, w, h, 8, on ? SERIES_COL[i] : COL_OFF);
    gfx->fillRect(x + 6, y + 8, 10, 14, on ? SERIES_COL[i] : COL_OFF);
    drawText(SERIES_LBL[i], x + 20, y + 8, 1, on ? COL_TEXT : COL_MUTED);
    char v[10];
    if (i == SOC_SERIES)
      snprintf(v, sizeof(v), "%d%%", seriesNow(i));
    else
      snprintf(v, sizeof(v), "%dW", seriesNow(i));
    drawText(v, x + 20, y + 24, 2, on ? COL_TEXT : COL_MUTED);
  }
}

static void drawChartScreen() {
  uint32_t t0 = millis();
  static uint32_t lastMs = 0;
  const int16_t W = gfx->width();
  gfx->fillScreen(COL_BG);
  drawText("Power History", 8, 10, 2, COL_TEXT);

  // Window-span selector (top-right).
  for (int i = 0; i < 3; i++) {
    int16_t bx, by, bw, bh;
    chartWindowRect(i, bx, by, bw, bh);
    bool sel = settings.chartWindow == i;
    gfx->fillRoundRect(bx, by, bw, bh, 6, sel ? ACC_WATER : COL_TILE);
    gfx->drawRoundRect(bx, by, bw, bh, 6, COL_MUTED);
    drawCenteredText(CHART_WIN_LBL[i], bx + bw / 2, by + bh / 2, 1,
                     sel ? COL_BG : COL_MUTED);
  }

  const int16_t PX0 = 44, PY0 = 40, PX1 = 474, PY1 = 240;
  const int16_t plotW = PX1 - PX0, plotH = PY1 - PY0;

  int n = powerlog_count();
  if (n < 2) {
    drawCenteredText("Collecting data...", W / 2, (PY0 + PY1) / 2, 2, COL_MUTED);
    drawChartLegend();
    return;
  }

  int nVis = n < chartWindowSamples() ? n : chartWindowSamples();
  int start = n - nVis;  // age-index of the first visible sample

  // Downsample to one representative sample per pixel column up front, so the
  // rest of the draw is O(plotW) regardless of how many samples are buffered
  // (the old per-sample auto-scale grew with the window and hitched the UI).
  for (int px = 0; px < plotW; px++) {
    int j = (nVis <= 1) ? 0 : (int)((int32_t)px * (nVis - 1) / (plotW - 1));
    const PwrSample &s = powerlog_at(start + j);
    g_col[px][0] = s.dcIn;
    g_col[px][1] = s.acIn;
    g_col[px][2] = s.dcOut;
    g_col[px][3] = s.acOut;
    g_col[px][4] = s.soc;
  }

  // Auto-scale watts (series 0..3) over the visible, enabled series.
  int yMax = 0;
  for (int px = 0; px < plotW; px++)
    for (int k = 0; k < 4; k++)
      if ((settings.chartSeriesMask & (1 << k)) && g_col[px][k] > yMax)
        yMax = g_col[px][k];
  if (yMax < 100) yMax = 100;
  int step = yMax <= 500 ? 100 : yMax <= 2000 ? 250 : 500;
  yMax = ((yMax + step - 1) / step) * step;

  // Axes.
  gfx->drawFastVLine(PX0, PY0, plotH, COL_OFF);
  gfx->drawFastHLine(PX0, PY1, plotW, COL_OFF);

  // Horizontal watts gridlines + value labels (0, 1/3, 2/3, max) left of the axis.
  drawText("W", 4, PY0 - 14, 1, COL_MUTED);
  for (int g = 0; g <= 3; g++) {
    int16_t gy = PY1 - (int16_t)((int32_t)plotH * g / 3);
    if (g > 0 && g < 3) gfx->drawFastHLine(PX0 + 1, gy, plotW - 1, COL_TILE);
    char yl[8];
    snprintf(yl, sizeof(yl), "%d", (int)((int32_t)yMax * g / 3));
    drawText(yl, 4, gy - 3, 1, COL_MUTED);
  }

  // Vertical time gridlines + "ago" labels: 10 min (1h), 30 min (6h), 2 h (24h).
  static const int winMin[3] = {60, 360, 1440};
  static const int gridMin[3] = {10, 30, 120};
  int wsel = settings.chartWindow > 2 ? 0 : settings.chartWindow;
  int nGrid = winMin[wsel] / gridMin[wsel];
  for (int d = 0; d <= nGrid; d++) {
    int16_t gx = PX0 + (int16_t)((int32_t)plotW * d / nGrid);
    if (d > 0 && d < nGrid) gfx->drawFastVLine(gx, PY0, plotH, COL_TILE);
    int agoMin = winMin[wsel] - d * gridMin[wsel];
    if (wsel != 0 && agoMin % 60 != 0) continue;  // 6h/24h: label whole hours
    char tb[8];
    if (agoMin == 0)
      strcpy(tb, "now");
    else if (wsel == 0)
      snprintf(tb, sizeof(tb), "%dm", agoMin);
    else
      snprintf(tb, sizeof(tb), "%dh", agoMin / 60);
    int tw = (int)strlen(tb) * 6;
    int16_t lx = (d == 0) ? PX0 : (d == nGrid) ? PX1 - tw : gx - tw / 2;
    drawText(tb, lx, PY1 + 5, 1, COL_MUTED);
  }

  // SoC rides its own 0..100% scale (right side); flag it so the % axis reads.
  if (settings.chartSeriesMask & (1 << SOC_SERIES))
    drawText("100%", PX1 - 24, PY0 - 3, 1, SERIES_COL[SOC_SERIES]);

  // One polyline per visible series, downsampled to plot-width columns. Watts
  // series use the auto-scaled yMax; SoC uses a fixed 0..100 scale.
  for (int k = 0; k < NSER; k++) {
    if (!(settings.chartSeriesMask & (1 << k))) continue;
    int scale = (k == SOC_SERIES) ? 100 : yMax;
    uint16_t col = SERIES_COL[k];
    int16_t prevY = 0;
    bool have = false;
    for (int px = 0; px < plotW; px++) {
      int v = g_col[px][k];
      if (v < 0) v = 0;
      if (v > scale) v = scale;
      int16_t Y = PY1 - (int16_t)((int32_t)v * plotH / scale);
      if (have) gfx->drawLine(PX0 + px - 1, prevY, PX0 + px, Y, col);
      prevY = Y;
      have = true;
    }
  }

  // Draw-time of the previous frame (compute only, excludes flush) for tuning.
  char ms[10];
  snprintf(ms, sizeof(ms), "%lums", (unsigned long)lastMs);
  drawText(ms, 250, 12, 1, COL_MUTED);

  drawChartLegend();
  lastMs = millis() - t0;
}

// A labelled settings row: label on the left, current value on the right,
// tappable to edit. Returns nothing; geometry is fixed by layout().
static void drawSettingRow(int16_t x, int16_t y, int16_t w, int16_t h,
                           const char *label, const char *value) {
  gfx->fillRoundRect(x, y, w, h, 8, COL_TILE);
  drawText(label, x + 14, y + 8, 1, COL_MUTED);
  drawText(strlen(value) ? value : "(tap to set)", x + 14, y + 22, 2,
           COL_TEXT);
  // chevron
  int16_t cxr = x + w - 22, cyr = y + h / 2;
  gfx->drawLine(cxr, cyr - 6, cxr + 6, cyr, COL_MUTED);
  gfx->drawLine(cxr + 6, cyr, cxr, cyr + 6, COL_MUTED);
}

// Shared compact row geometry for the Bluetti settings page.
static int16_t srowY(int i) { return nameRowY + i * (nameRowH + 4); }

// One tappable toggle row (label left, ON/OFF pill right) for the Bluetti page.
static void drawBtToggleRow(int i, const char *label, bool on) {
  int16_t y = srowY(i);
  gfx->fillRoundRect(nameRowX, y, nameRowW, nameRowH, 8, COL_TILE);
  drawText(label, nameRowX + 14, y + (nameRowH - 16) / 2, 2, COL_TEXT);
  const int16_t pw = 66, ph = 26, px = nameRowX + nameRowW - pw - 12,
                py = y + (nameRowH - ph) / 2;
  gfx->fillRoundRect(px, py, pw, ph, ph / 2, on ? COL_ON : COL_OFF);
  drawCenteredText(on ? "ON" : "OFF", px + pw / 2, py + ph / 2, 2, COL_BG);
}

// Stepper button geometry for Bluetti settings row i ([-] value [+] at right).
static const int16_t BT_STEP = 30;
static void btStepperRects(int i, int16_t &minusX, int16_t &plusX, int16_t &y,
                           int16_t &s) {
  int16_t ry = srowY(i);
  s = BT_STEP;
  y = ry + (nameRowH - s) / 2;
  plusX = nameRowX + nameRowW - 12 - s;
  minusX = plusX - s - 70;  // 70px value field sits between the two buttons
}

// Label left, "-" value "+" steppers right.
static void drawBtStepperRow(int i, const char *label, const char *value) {
  int16_t ry = srowY(i);
  gfx->fillRoundRect(nameRowX, ry, nameRowW, nameRowH, 8, COL_TILE);
  drawText(label, nameRowX + 14, ry + (nameRowH - 16) / 2, 2, COL_TEXT);
  int16_t minusX, plusX, y, s;
  btStepperRects(i, minusX, plusX, y, s);
  gfx->fillRoundRect(minusX, y, s, s, 6, COL_TILE_DN);
  drawCenteredText("-", minusX + s / 2, y + s / 2 - 1, 3, COL_TEXT);
  gfx->fillRoundRect(plusX, y, s, s, 6, COL_TILE_DN);
  drawCenteredText("+", plusX + s / 2, y + s / 2 - 1, 3, COL_TEXT);
  drawCenteredText(value, (minusX + s + plusX) / 2, ry + nameRowH / 2, 2,
                   COL_TEXT);
}

// Label left, tappable value pill right (cycles on tap).
static void drawBtValueRow(int i, const char *label, const char *value) {
  int16_t y = srowY(i);
  gfx->fillRoundRect(nameRowX, y, nameRowW, nameRowH, 8, COL_TILE);
  drawText(label, nameRowX + 14, y + (nameRowH - 16) / 2, 2, COL_TEXT);
  const int16_t pw = 96, ph = 26, px = nameRowX + nameRowW - pw - 12,
                py = y + (nameRowH - ph) / 2;
  gfx->fillRoundRect(px, py, pw, ph, 6, COL_TILE_DN);
  drawCenteredText(value, px + pw / 2, py + ph / 2, 2, COL_TEXT);
}

static const char *btTimeoutLabel(int e) {
  switch (e) {
    case 2: return "30 sec";
    case 3: return "1 min";
    case 4: return "5 min";
    case 5: return "Never";
    default: return "?";
  }
}

// Bluetti settings sub-page (opened via the gear icon on the Power screen):
// a top utility row (history chart + app-release, always available), then
// output ECO, charge limit, screen timeout, and BLE pairing (once live data
// has arrived). No title -- self-explanatory from the gear that opens it.
// Back = left->right swipe.
static void drawBtSettings() {
  const int16_t W = gfx->width();
  gfx->fillScreen(COL_BG);

  // History + app-release: available regardless of connection state, so
  // drawn (and handled, in ui_handle_touch) before the power_valid() gate.
  drawChartButton();
  drawReleaseButton((int)bluetti_release_remaining());

  // Don't show/allow editing the rest of the controls until live data has
  // arrived.
  if (!power_valid()) {
    const int16_t H = gfx->height();
    drawCenteredText("Connecting to the Bluetti...", W / 2, H / 2 - 8, 2,
                     COL_MUTED);
    drawCenteredText("Settings unlock once data is received", W / 2, H / 2 + 18,
                     1, COL_MUTED);
    return;
  }
  drawBtToggleRow(0, "AC ECO", power.acEco);
  drawBtToggleRow(1, "DC ECO", power.dcEco);
  drawBtToggleRow(2, "Power Lifting", power.powerLift);
  char cl[8];
  snprintf(cl, sizeof(cl), "%d%%", power.chargeLimit);
  drawBtStepperRow(3, "Charge Limit", cl);
  drawBtValueRow(4, "Screen Timeout", btTimeoutLabel(power.screenTimeout));
  drawSettingRow(nameRowX, srowY(5), nameRowW, nameRowH, "Pairing (BLE MAC)",
                 strlen(settings.bluettiMac) ? settings.bluettiMac : "Auto");
  // AC output voltage + frequency, with a clear gap below the last row so it
  // doesn't crowd the Pairing row above it.
  char ac[40];
  if (power.acOn && power.acOutDV > 0)
    snprintf(ac, sizeof(ac), "AC out:  %d.%d V    %d.%d Hz", power.acOutDV / 10,
             power.acOutDV % 10, power.acOutFreqDHz / 10, power.acOutFreqDHz % 10);
  else
    snprintf(ac, sizeof(ac), "AC output off");
  drawCenteredText(ac, W / 2, srowY(5) + nameRowH + 14, 1, COL_MUTED);
}

// ===== Public ===============================================================
// --- Pending-write ("Applying…") feedback ---------------------------------
// Bluetti control writes confirm asynchronously (read-back ~1-2s), so a tap
// doesn't change the control immediately. Show an "Applying…" badge from the
// tap until the next poll confirms the new value (or a short safety timeout).
static bool g_writePending = false;
static uint32_t g_pendingFetch = 0;  // power.fetchedMs captured at the tap
static uint32_t g_pendingStart = 0;

static void notePendingWrite() {
  g_writePending = true;
  g_pendingFetch = power.fetchedMs;
  g_pendingStart = millis();
}

// Scale an RGB565 colour's brightness by num/den (for the spinner trail fade).
static uint16_t dim565(uint16_t c, int num, int den) {
  int r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
  r = r * num / den;
  g = g * num / den;
  b = b * num / den;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Medium centred spinner shown while a Bluetti write is being confirmed: a ring
// of dots with a brightness trail that rotates.
static void drawPendingSpinner() {
  const int16_t cx = gfx->width() / 2, cy = gfx->height() / 2;
  const int N = 12;
  const int16_t R = 26, dotR = 4;
  gfx->fillCircle(cx, cy, R + dotR + 9, COL_BAR);  // clean backing disc
  int head = (int)((millis() / 90) % N);
  for (int i = 0; i < N; i++) {
    float a = (float)i / N * 2.0f * (float)PI - (float)PI / 2;
    int16_t x = cx + (int16_t)(cosf(a) * R);
    int16_t y = cy + (int16_t)(sinf(a) * R);
    int age = (head - i + N) % N;  // 0 = head (brightest), fading backwards
    gfx->fillCircle(x, y, dotR, dim565(COL_TEXT, N - age, N));
  }
}

void ui_draw() {
  if (keyboard_active()) {  // modal keyboard owns the screen
    keyboard_draw();
    return;
  }
  switch (current) {
    case POWER: drawPowerScreen(); break;
    case POWER_CHART: drawChartScreen(); break;
    case BT_SETTINGS: drawBtSettings(); break;
  }
  if (g_writePending && (current == POWER || current == BT_SETTINGS))
    drawPendingSpinner();
  gfx->flush();
}

void ui_init() {
  layout();
  current = POWER;
  ui_draw();
}

void ui_tick() {
  // While a Bluetti write is pending, own the screen: animate the "Applying…"
  // badge, and clear it (repaint with the confirmed value) once the next poll
  // lands or a short safety timeout elapses.
  if (g_writePending) {
    if (power.fetchedMs != g_pendingFetch || millis() - g_pendingStart > 6000) {
      g_writePending = false;
      ui_draw();
    } else if (current == POWER || current == BT_SETTINGS) {
      static uint32_t lastAnim = 0;
      if (millis() - lastAnim >= 90) {
        lastAnim = millis();
        drawPendingSpinner();
        gfx->flush();
      }
    }
    return;
  }

  if (keyboard_active()) return;

  // Redraw the Power screen (status bar + data) as fresh readings arrive.
  if (current == POWER) {
    if (pwrConfirm) return;  // keep the confirm overlay stable
    // Tick the app-release countdown (and refresh once when it resumes).
    static int lastRem = -1;
    int rem = (int)bluetti_release_remaining();
    if (rem > 0) {
      if (rem != lastRem) {
        lastRem = rem;
        ui_draw();
      }
      return;
    }
    if (lastRem > 0) {  // just resumed
      lastRem = 0;
      ui_draw();
    }

    if (!power_valid()) {
      // Animate the Bluetooth icon on the offline screen while connecting;
      // redraw (full, but cheap -- it's just two lines of text + an icon)
      // on any link-state change too.
      static BluettiConn lastBtIcon = (BluettiConn)-1;
      static uint32_t lastBtAnim = 0;
      BluettiConn bti = bluetti_state();
      bool tick = bti == BTC_CONNECTING && millis() - lastBtAnim > 400;
      if (tick) {
        lastBtAnim = millis();
        btAnimPhase++;
      }
      if (bti != lastBtIcon || tick) {
        lastBtIcon = bti;
        ui_draw();
      }
      return;
    }

    static PowerStatus lastPs = (PowerStatus)-1;
    static int lastSoc = -999, lastSum = -999999, lastFlags = -1, lastTtf = -1;
    int sum = power.dcInW + power.acInW + power.dcOutW + power.acOutW +
              power.chargeMode * 7;
    int flags = (power.acOn ? 1 : 0) | (power.dcOn ? 2 : 0) |
                (power.charging ? 4 : 0);
    if (power.status != lastPs || power.soc != lastSoc || sum != lastSum ||
        flags != lastFlags || power.ttfMin != lastTtf) {
      lastPs = power.status;
      lastSoc = power.soc;
      lastSum = sum;
      lastFlags = flags;
      lastTtf = power.ttfMin;
      ui_draw();
    }
    return;
  }

  // Redraw the history chart when a new sample lands (taps redraw directly).
  if (current == POWER_CHART) {
    static int lastCount = -1;
    int c = powerlog_count();
    if (c != lastCount) {
      lastCount = c;
      ui_draw();
    }
    return;
  }

  // Redraw the Bluetti settings sub-page when a toggle is confirmed by a poll.
  if (current == BT_SETTINGS) {
    static int lastBt = -1, lastBt2 = -1;
    static bool lastValid = false;
    bool pv = power_valid();  // placeholder <-> controls transition
    int b = (power.acEco ? 1 : 0) | (power.dcEco ? 2 : 0) |
            (power.powerLift ? 4 : 0) | (power.chargeLimit << 3) |
            (power.screenTimeout << 10);
    int b2 = (power.acOn ? 1 : 0) | (power.acOutDV << 1) |
             (power.acOutFreqDHz << 14);  // AC info line
    if (b != lastBt || b2 != lastBt2 || pv != lastValid) {
      lastBt = b;
      lastBt2 = b2;
      lastValid = pv;
      ui_draw();
    }
    return;
  }
}

void ui_handle_touch(int16_t x, int16_t y) {
  // Modal keyboard intercepts everything while open.
  if (keyboard_active()) {
    if (keyboard_handle_touch(x, y)) {  // committed or cancelled
      if (keyboard_committed() && editTarget == EDIT_BTMAC) {
        strncpy(settings.bluettiMac, keyboard_value(),
                sizeof(settings.bluettiMac) - 1);
        settings.bluettiMac[sizeof(settings.bluettiMac) - 1] = '\0';
        settings_save();
        power.status = PWR_NONE;  // drop stale data; BLE task picks up new MAC
        current = BT_SETTINGS;  // return to the Bluetti page
      }
      editTarget = EDIT_NONE;
      ui_draw();
    }
    return;
  }

  // Power screen: gear -> Bluetti settings, output-card toggles, app-release,
  // and the confirm step.
  if (current == POWER) {
    // Settings gear (top-right of the status bar).
    if (inRect(x, y, gearX, gearY, gearW, gearH)) {
      current = BT_SETTINGS;
      ui_draw();
      return;
    }
    // App mode: no dedicated button anymore (release/resume both live on
    // Settings) -- any tap on the screen resumes early.
    if (bluetti_release_remaining() > 0) {
      bluetti_release(0);
      ui_draw();
      return;
    }
    if (pwrConfirm) {
      int16_t cancelX, confirmX, by, bw, bh;
      powerConfirmRects(cancelX, confirmX, by, bw, bh);
      if (inRect(x, y, confirmX, by, bw, bh)) {
        bool ac = (pwrConfirm == 1);
        bluetti_set_output(ac, pwrConfirmTarget);  // UI updates once a poll confirms it
        pwrConfirm = 0;
        notePendingWrite();
        ui_draw();
        return;
      }
      if (inRect(x, y, cancelX, by, bw, bh)) {
        pwrConfirm = 0;
        ui_draw();
        return;
      }
      return;  // swallow other taps while confirming
    }
    // Output cards toggle their output (DC OUT = card 2, AC OUT = card 3).
    if (power_valid()) {
      // Charge-mode button: cycle the three main modes (skip Custom).
      int16_t mx, my, mw, mh;
      powerChargeBtnRect(mx, my, mw, mh);
      if (inRect(x, y, mx, my, mw, mh)) {
        int nm = (power.chargeMode == 0) ? 1 : (power.chargeMode == 1) ? 2 : 0;
        bluetti_write_reg(2020, nm);  // UI updates once a poll confirms it
        notePendingWrite();
        ui_draw();
        return;
      }
      int16_t cx, cy, cw, ch;
      powerCardRect(2, cx, cy, cw, ch);
      if (inRect(x, y, cx, cy, cw, ch)) {
        pwrConfirm = 2;
        pwrConfirmTarget = !power.dcOn;
        ui_draw();
        return;
      }
      powerCardRect(3, cx, cy, cw, ch);
      if (inRect(x, y, cx, cy, cw, ch)) {
        pwrConfirm = 1;
        pwrConfirmTarget = !power.acOn;
        ui_draw();
        return;
      }
    }
    return;
  }

  // Power history chart: legend chips toggle each series, window buttons set span.
  if (current == POWER_CHART) {
    for (int i = 0; i < NSER; i++) {
      int16_t bx, by, bw, bh;
      chartLegendRect(i, bx, by, bw, bh);
      if (inRect(x, y, bx, by, bw, bh)) {
        settings.chartSeriesMask ^= (1 << i);
        settings_save();
        ui_draw();
        return;
      }
    }
    for (int i = 0; i < 3; i++) {
      int16_t bx, by, bw, bh;
      chartWindowRect(i, bx, by, bw, bh);
      if (inRect(x, y, bx, by, bw, bh)) {
        settings.chartWindow = i;
        settings_save();
        ui_draw();
        return;
      }
    }
    return;
  }

  // Bluetti settings sub-page: history/release utility row, toggles,
  // charge-limit stepper, timeout cycle.
  if (current == BT_SETTINGS) {
    // History + app-release: available regardless of connection state.
    {
      int16_t bx, by, bw, bh;
      btChartBtnRect(bx, by, bw, bh);
      if (inRect(x, y, bx, by, bw, bh)) {
        current = POWER_CHART;
        ui_draw();
        return;
      }
      btReleaseBtnRect(bx, by, bw, bh);
      if (inRect(x, y, bx, by, bw, bh)) {
        bluetti_release(90);
        ui_draw();
        return;
      }
    }
    if (!power_valid()) return;  // remaining controls disabled until live data arrives
    struct { int reg; bool *val; } rows[3] = {
        {2017, &power.acEco}, {2014, &power.dcEco}, {2021, &power.powerLift}};
    for (int i = 0; i < 3; i++) {
      if (inRect(x, y, nameRowX, srowY(i), nameRowW, nameRowH)) {
        bool nv = !*rows[i].val;
        bluetti_write_reg(rows[i].reg, nv ? 1 : 0);  // UI updates once a poll confirms it
        notePendingWrite();
        ui_draw();
        return;
      }
    }
    // Charge limit steppers (row 3): 5 % steps, clamped 50..100. Hi byte = %.
    int16_t minusX, plusX, sy, ss;
    btStepperRects(3, minusX, plusX, sy, ss);
    bool hitMinus = inRect(x, y, minusX, sy, ss, ss);
    bool hitPlus = inRect(x, y, plusX, sy, ss, ss);
    if (hitMinus || hitPlus) {
      int nc = power.chargeLimit + (hitPlus ? 5 : -5);
      if (nc < 50) nc = 50;
      if (nc > 100) nc = 100;
      if (nc != power.chargeLimit) {
        bluetti_write_reg(2083, (uint16_t)(nc << 8));  // UI updates once a poll confirms it
        notePendingWrite();
        ui_draw();
      }
      return;
    }
    // Screen timeout (row 4): tap to cycle 30s -> 1m -> 5m -> Never.
    if (inRect(x, y, nameRowX, srowY(4), nameRowW, nameRowH)) {
      int e = (power.screenTimeout >= 5 || power.screenTimeout < 2)
                  ? 2
                  : power.screenTimeout + 1;
      bluetti_write_reg(2067, (uint16_t)e);  // UI updates once a poll confirms it
      notePendingWrite();
      ui_draw();
      return;
    }
    // Pairing (row 5): edit the BLE MAC via the keyboard.
    if (inRect(x, y, nameRowX, srowY(5), nameRowW, nameRowH)) {
      editTarget = EDIT_BTMAC;
      keyboard_open("Bluetti MAC (blank = auto)", settings.bluettiMac,
                    sizeof(settings.bluettiMac) - 1, false);
      ui_draw();
      return;
    }
    return;
  }
}

void ui_handle_drag(int16_t, int16_t) {}  // no continuous controls in this build

void ui_handle_release(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  if (current == POWER) return;
  int dx = x1 - x0, dy = y1 - y0;
  if (abs(dx) < 60 || abs(dx) < abs(dy)) return;  // not a clear horizontal swipe

  // Left->right swipe = go back to the Power screen (the only destination left).
  if (dx > 0) {
    pwrConfirm = 0;
    current = POWER;
    ui_draw();
  }
}
