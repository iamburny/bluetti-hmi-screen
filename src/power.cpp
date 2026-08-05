#include "power.h"
#include <Arduino.h>

// The shared telemetry struct. It is populated by the native BLE client task in
// bluetti.cpp (which connects to the Elite 300, runs the encrypted handshake,
// and reads the registers). The UI consumes it via power_valid()/power_soc().
PowerData power = {};

bool power_valid() {
  // Tolerate a few missed BLE polls (the link is briefly busy when the phone
  // app connects) before showing "offline".
  return power.status == PWR_OK && (millis() - power.fetchedMs) < 60000UL;
}

int power_soc() { return power_valid() ? power.soc : 0; }
