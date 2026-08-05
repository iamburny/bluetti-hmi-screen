#include "sdcard.h"
#include <Arduino.h>
#include <SD_MMC.h>

// SD card pins (verified from vendor pincfg.h): SD_MMC 1-bit.
#define SD_CLK 12
#define SD_CMD 11
#define SD_D0 13

static bool s_ready = false;

bool sd_begin() {
  if (s_ready) return true;
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true /* 1-bit */)) {
    Serial.println("[sd] mount failed");
    return false;
  }
  s_ready = true;
  return true;
}
