#pragma once
#include <Arduino_GFX_Library.h>

// Screen orientation, shared by display (framebuffer) and touch (coord mapping).
//   0 / 2 -> portrait  (320x480)
//   1 / 3 -> landscape (480x320)
// If touch lands mirrored after a change, switch 1<->3 (or 0<->2).
#define SCREEN_ROTATION 1

// Native (rotation-0) panel dimensions = the touch controller's coord space.
#define PANEL_NATIVE_W 320
#define PANEL_NATIVE_H 480

// Canvas-backed GFX target (PSRAM framebuffer). Draw into this, then gfx->flush().
extern Arduino_GFX *gfx;

// Bring up QSPI display + backlight and clear the screen.
void display_init();

// Set backlight brightness 0..255 (PWM on GPIO 1).
void display_backlight(uint8_t level);

// `size` (1..5) selects one of a small set of custom FreeSans fonts rather
// than scaling the built-in glyphs (see applyTextSize() in display.cpp) —
// 1/2 = regular labels, 3/4 = bold, 5 = bold at 2x for the headline SoC %.

// Draw text centered on (cx, cy). Arduino_GFX has no drawCentreString, so we
// compute the offset from getTextBounds() ourselves.
void drawCenteredText(const char *s, int16_t cx, int16_t cy, uint8_t size,
                      uint16_t color);

// Draw left/top-anchored text at (x, y) — (x, y) is the top-left of the
// glyph box regardless of font (custom GFXfonts anchor to the baseline
// internally; this compensates so callers don't have to care).
void drawText(const char *s, int16_t x, int16_t y, uint8_t size,
              uint16_t color);

// Bounding-box size (pixels) `s` would occupy at `size`, without drawing it.
// Use this instead of guessing glyph metrics when hand-composing text at
// different sizes (e.g. a big number + a smaller unit suffix) — FreeSans is
// proportional, not monospace.
void measureText(const char *s, uint8_t size, int16_t &w, int16_t &h);
