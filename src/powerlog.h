#pragma once
#include <Arduino.h>

// Rolling history of Bluetti power flows for the on-device chart screen and an
// SD-card CSV log. Fed from the main loop off the published `power` struct, so
// it never touches the timing-sensitive BLE task.

struct PwrSample {                   // 12 bytes
  uint32_t t;                        // uptime secs (millis()/1000 at sample time)
  int16_t soc;                       // %
  int16_t dcIn, acIn, dcOut, acOut;  // watts
};

// Allocate the PSRAM ring buffer and mount the SD card. Call once in setup().
void powerlog_init();

// Capture a sample when a fresh poll arrives (rate-limited). Call from loop().
void powerlog_tick();

// Samples currently held (0..RING_N).
int powerlog_count();

// Sample by age: i = 0 is oldest, count-1 is newest. Out-of-range -> zeroed.
const PwrSample &powerlog_at(int i);
