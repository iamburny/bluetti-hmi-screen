// Pure, hardware-free touch logic. No Arduino/Wire deps so it can be unit-tested
// on the host (pio test -e native) and shared with the firmware.
#pragma once
#include <stdint.h>

static inline int logic_clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Parse an AXS15231 touch report (14-byte read). Returns true and fills the raw
// native-frame coords when a valid single/double touch is present.
//   valid: len >= 6, buf[0]==0, buf[1] (count) in 1..2
//   rawX = ((buf[2] & 0x0F) << 8) | buf[3]
//   rawY = ((buf[4] & 0x0F) << 8) | buf[5]
static inline bool parseAxsTouch(const uint8_t *buf, int len, int16_t &rawX,
                                 int16_t &rawY) {
  if (!buf || len < 6) return false;
  uint8_t cnt = buf[1];
  if (cnt == 0 || cnt > 2 || buf[0] != 0) return false;
  rawX = (int16_t)(((buf[2] & 0x0F) << 8) | buf[3]);
  rawY = (int16_t)(((buf[4] & 0x0F) << 8) | buf[5]);
  return true;
}

// Map raw (rotation-0) controller coords to screen coords for the active
// rotation, clamped to the rotated screen size. panelW/panelH are the native
// panel dims (320x480); screenW/screenH are the rotated dims.
static inline void mapTouch(int rawX, int rawY, int rotation, int panelW,
                            int panelH, int screenW, int screenH, int16_t &outX,
                            int16_t &outY) {
  int x, y;
  switch (rotation) {
    case 1:
      x = rawY;
      y = (panelW - 1) - rawX;
      break;
    case 2:
      x = (panelW - 1) - rawX;
      y = (panelH - 1) - rawY;
      break;
    case 3:
      x = (panelH - 1) - rawY;
      y = rawX;
      break;
    default:  // 0
      x = rawX;
      y = rawY;
      break;
  }
  outX = (int16_t)logic_clampi(x, 0, screenW - 1);
  outY = (int16_t)logic_clampi(y, 0, screenH - 1);
}
