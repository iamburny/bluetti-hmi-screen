#include "touch.h"
#include "display.h"  // for gfx->width()/height()
#include "logic/touch_map.h"
#include <Wire.h>

// Cap-touch pins (verified from vendor pincfg.h): SDA=4, SCL=8. This panel's
// touch has NO reset line (RST=-1) and INT=GPIO3 (unused here). NOTE: CLAUDE.md
// wrongly lists touch RST=12 / INT=11 — those are actually the SD card's
// CLK(12)/CMD(11), so we must NOT drive them here or it breaks the SD card.
#define TOUCH_SDA 4
#define TOUCH_SCL 8
#define AXS_ADDR 0x3B  // AXS15231 integrated touch controller

// NOTE: we talk to the AXS15231 touch directly over I2C. bb_captouch has the
// read code but its auto-detect for this chip is gated behind #ifdef FUTURE
// (disabled), so it never recognizes the controller. The read command/parse
// below mirror bb_captouch's AXS15231 path.

void touch_init() {
  Wire.begin(TOUCH_SDA, TOUCH_SCL);
  Wire.setClock(400000);
  Wire.setTimeout(1000);

  // Probe: does anything ACK at 0x3B?
  Wire.beginTransmission(AXS_ADDR);
  int err = Wire.endTransmission();
  Serial.printf("[touch] AXS15231 @0x3B probe: %s (err=%d)\n",
                err == 0 ? "ACK ok" : "NO ACK", err);
}

bool touch_get(int16_t &x, int16_t &y) {
  static const uint8_t readCmd[8] = {0xb5, 0xab, 0xa5, 0x5a, 0, 0, 0, 0x08};
  uint8_t buf[14] = {0};

  Wire.beginTransmission(AXS_ADDR);
  Wire.write(readCmd, 8);
  if (Wire.endTransmission() != 0) return false;

  int n = Wire.requestFrom(AXS_ADDR, 14);
  if (n < 6) return false;
  for (int i = 0; i < n && i < 14; i++) buf[i] = Wire.read();

  // Parse the raw (rotation-0) coords, then rotate/clamp to the screen frame.
  // Both steps are pure helpers in logic/touch_map.h (unit-tested).
  int16_t rx, ry;
  if (!parseAxsTouch(buf, n, rx, ry)) return false;
  mapTouch(rx, ry, SCREEN_ROTATION, PANEL_NATIVE_W, PANEL_NATIVE_H,
           gfx->width(), gfx->height(), x, y);
  return true;
}
