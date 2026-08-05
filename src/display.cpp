#include "display.h"
#include "fonts/FreeSans9pt7b.h"
#include "fonts/FreeSans12pt7b.h"
#include "fonts/FreeSansBold18pt7b.h"
#include "fonts/FreeSansBold24pt7b.h"

// Backlight pin for the JC3248W535EN (per CLAUDE.md / library board def).
#define GFX_BL 1

// QSPI bus + AXS15231B panel, pins verbatim from the library's known-good
// JC3248W535 board definition (Arduino_GFX_dev_device.h). Note the 320x480
// type1 init sequence is required for this panel (the default is 180x640).
static Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    45 /* CS */, 47 /* SCK */, 21 /* D0 */, 48 /* D1 */, 40 /* D2 */, 39 /* D3 */);

// IMPORTANT: the panel driver stays at rotation 0 / native 320x480. Rotating
// the driver garbles Arduino_Canvas::flush() (which always ships the native
// WIDTH x HEIGHT buffer). Orientation is done on the CANVAS instead.
static Arduino_GFX *g = new Arduino_AXS15231B(
    bus, GFX_NOT_DEFINED /* RST */, 0 /* rotation */, false /* IPS */,
    320 /* width */, 480 /* height */, 0, 0, 0, 0,
    axs15231b_320480_type1_init_operations,
    sizeof(axs15231b_320480_type1_init_operations));

// Native 320x480 PSRAM buffer; the LAST arg (SCREEN_ROTATION) rotates the
// logical drawing coords so width()/height() reflect the orientation, while
// flush() still ships the native buffer to the rotation-0 panel.
Arduino_GFX *gfx = new Arduino_Canvas(320, 480, g, 0, 0, SCREEN_ROTATION);

void display_init() {
  gfx->begin();

  // Backlight on PWM so we can dim / fade (used by the photo screen saver).
  ledcAttach(GFX_BL, 5000 /* Hz */, 8 /* bits */);
  display_backlight(255);  // full on

  gfx->fillScreen(RGB565_BLACK);
  gfx->flush();
}

void display_backlight(uint8_t level) { ledcWrite(GFX_BL, level); }

// Maps the app's 1..5 size scale onto a small set of custom FreeSans fonts
// instead of scaling the built-in glyphs. Size 5 additionally doubles the
// biggest font via setTextSize() for extra visual weight on the headline
// SoC %; every other size renders at its native point size.
static void applyTextSize(uint8_t size) {
  switch (size) {
    case 1: gfx->setFont(&FreeSans9pt7b); gfx->setTextSize(1); break;
    case 2: gfx->setFont(&FreeSans12pt7b); gfx->setTextSize(1); break;
    case 3: gfx->setFont(&FreeSansBold18pt7b); gfx->setTextSize(1); break;
    case 4: gfx->setFont(&FreeSansBold24pt7b); gfx->setTextSize(1); break;
    default: gfx->setFont(&FreeSansBold24pt7b); gfx->setTextSize(2); break;
  }
}

void drawCenteredText(const char *s, int16_t cx, int16_t cy, uint8_t size,
                      uint16_t color) {
  int16_t x1, y1;
  uint16_t w, h;
  applyTextSize(size);
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(color);
  // Subtract the bounding-box offset (x1/y1) so glyph metrics are accounted for.
  gfx->setCursor(cx - (w / 2) - x1, cy - (h / 2) - y1);
  gfx->print(s);
}

void drawText(const char *s, int16_t x, int16_t y, uint8_t size,
              uint16_t color) {
  int16_t x1, y1;
  uint16_t w, h;
  applyTextSize(size);
  // Custom GFXfonts anchor the cursor to the text baseline, not the top-left
  // like the built-in font did. Shift by the bounding-box offset so (x, y)
  // keeps meaning "top-left of the glyph box" for every call site.
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  gfx->setTextColor(color);
  gfx->setCursor(x - x1, y - y1);
  gfx->print(s);
}

void measureText(const char *s, uint8_t size, int16_t &w, int16_t &h) {
  int16_t x1, y1;
  uint16_t uw, uh;
  applyTextSize(size);
  gfx->getTextBounds(s, 0, 0, &x1, &y1, &uw, &uh);
  w = (int16_t)uw;
  h = (int16_t)uh;
}
