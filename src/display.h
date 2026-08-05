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

// Draw text centered on (cx, cy). Arduino_GFX has no drawCentreString, so we
// compute the offset from getTextBounds() ourselves.
void drawCenteredText(const char *s, int16_t cx, int16_t cy, uint8_t size,
                      uint16_t color);

// Draw left/top-anchored text at (x, y).
void drawText(const char *s, int16_t x, int16_t y, uint8_t size,
              uint16_t color);
