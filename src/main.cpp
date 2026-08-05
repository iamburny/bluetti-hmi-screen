/*
 * Bluetti HMI Screen
 *
 * Touch dashboard for the Guition JC3248W535EN (ESP32-S3): a native BLE client
 * for the Bluetti Elite 300 power station. On-device decrypted "v2" protocol,
 * no bridge/cloud/licence. Fully offline (no WiFi) — BLE has the radio to
 * itself. The Power screen is the home screen; its gear icon opens Bluetti
 * settings, and a history-chart button opens the power-flow chart.
 *
 * Modules: display (AXS15231B + canvas), touch (AXS15231 I2C), bluetti +
 * bluetti_crypt (native BLE client + on-device decrypt), powerlog (history
 * ring + SD CSV), settings + keyboard (BLE-MAC entry), ui (screen states).
 */

#include <Arduino.h>
#include "display.h"
#include "touch.h"
#include "ui.h"
#include "settings.h"
#include "bluetti.h"
#include "powerlog.h"

void setup() {
  Serial.begin(115200);

  settings_load();  // Bluetti MAC / chart prefs from NVS (before UI draws)
  display_init();
  ui_init();        // draw UI first so it shows even if touch init misbehaves
  touch_init();
  bluetti_begin();  // start the native Bluetti BLE telemetry task
  powerlog_init();  // PSRAM history ring + SD CSV log for the power chart

  Serial.println("[hmi] ready");
}

void loop() {
  static bool pressed = false;  // rising-edge latch: one action per touch
  static uint8_t releaseCount = 0;
  int16_t x, y;

  static int16_t pressX, pressY, lastX, lastY;  // for swipe gestures

  if (touch_get(x, y)) {
    releaseCount = 0;
    lastX = x;
    lastY = y;
    if (!pressed) {
      pressed = true;
      pressX = x;
      pressY = y;
      ui_handle_touch(x, y);  // discrete actions (rising edge)
    } else {
      ui_handle_drag(x, y);  // continuous controls while held
    }
  } else if (pressed && ++releaseCount >= 3) {
    // Require a few consecutive empty reads before treating the finger as
    // lifted; the AXS15231 occasionally drops a frame mid-press, which would
    // otherwise re-trigger and double-toggle.
    pressed = false;
    ui_handle_release(pressX, pressY, lastX, lastY);  // swipe detection
  }

  powerlog_tick();  // capture a power sample when a fresh BLE poll arrives
  ui_tick();        // refresh time-based UI (status bar, pending-write spinner)
  delay(10);
}
