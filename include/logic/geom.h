// Pure geometry helpers (hardware-free; unit-testable + shared with firmware).
#pragma once
#include <stdint.h>

// Point-in-rect hit test: [x, x+w) × [y, y+h).
static inline bool inRect(int16_t px, int16_t py, int16_t x, int16_t y,
                          int16_t w, int16_t h) {
  return px >= x && px < (x + w) && py >= y && py < (y + h);
}
