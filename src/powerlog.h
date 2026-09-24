#pragma once
#include <Arduino.h>

// Rolling history of Bluetti power flows for the on-device chart screen and an
// SD-card CSV log. Fed from the main loop off the published `power` struct, so
// it never touches the timing-sensitive BLE task.

struct PwrSample {                   // 14 bytes
  uint32_t t;                        // uptime secs (millis()/1000 at sample time)
  int16_t soc;                       // %
  int16_t dcIn, acIn, dcOut, acOut;  // watts
  int16_t reg156;                    // raw reg 156 (unidentified); 0 in
                                      // samples replayed from a pre-existing
                                      // CSV logged before this field existed
};

// Allocate the PSRAM ring buffer and mount the SD card. Call once in setup().
void powerlog_init();

// Capture a sample when a fresh poll arrives (rate-limited). Call from loop().
void powerlog_tick();

// Samples currently held (0..RING_N).
int powerlog_count();

// Samples pushed since boot. Unlike powerlog_count() it keeps changing once
// the ring is full, so it's what to watch for "a new sample landed".
uint32_t powerlog_total();

// Sample by age: i = 0 is oldest, count-1 is newest. Out-of-range -> zeroed.
const PwrSample &powerlog_at(int i);
