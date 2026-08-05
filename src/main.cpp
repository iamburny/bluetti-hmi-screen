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
  bluetti_set_poll_ms(settings.pollMs);  // tuned rate from NVS, before the task starts
  bluetti_set_gap_ms(settings.gapMs);
  bluetti_begin();  // start the native Bluetti BLE telemetry task
  powerlog_init();  // PSRAM history ring + SD CSV log for the power chart

  // Says whether we'll go straight to a known address or have to scan for it.
  // Deliberately doesn't print the address itself.
  Serial.printf("[hmi] Bluetti address: %s\n",
                strlen(settings.bluettiMac) >= 17 ? "saved (skips scan)"
                                                  : "none yet (will scan)");
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

  // If the BLE task discovered the Bluetti by scanning, persist the address so
  // later connects skip the ~5s scan. Done here rather than in that task to
  // keep NVS writes on a single thread.
  char learnedMac[18];
  if (bluetti_take_learned_mac(learnedMac, sizeof(learnedMac))) {
    strncpy(settings.bluettiMac, learnedMac, sizeof(settings.bluettiMac) - 1);
    settings.bluettiMac[sizeof(settings.bluettiMac) - 1] = '\0';
    settings_save();
    Serial.printf("[hmi] saved Bluetti address from scan: %s\n",
                  settings.bluettiMac);
  }

  powerlog_tick();  // capture a power sample when a fresh BLE poll arrives
  ui_tick();        // refresh time-based UI (pending-write spinner, live data)
  delay(10);
}
