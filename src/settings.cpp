#include "settings.h"
#include <Preferences.h>

Settings settings;

static Preferences prefs;
static const char *NS = "vanctl";  // NVS namespace

static void copyStr(char *dst, const String &src, size_t cap) {
  strncpy(dst, src.c_str(), cap - 1);
  dst[cap - 1] = '\0';
}

void settings_load() {
  prefs.begin(NS, true);  // read-only
  copyStr(settings.bluettiMac, prefs.getString("btmac", ""),
          sizeof(settings.bluettiMac));
  settings.chartSeriesMask = prefs.getUChar("chMask", 0x3F);  // all 6 traces on
  settings.chartWindow = prefs.getUChar("chWin", 0);          // default 1h
  settings.pollMs = prefs.getUShort("pollMs", 3000);          // BLE refresh rate
  settings.gapMs = prefs.getUChar("gapMs", 40);               // inter-command gap
  prefs.end();
}

void settings_save() {
  prefs.begin(NS, false);  // read-write
  prefs.putString("btmac", settings.bluettiMac);
  prefs.putUChar("chMask", settings.chartSeriesMask);
  prefs.putUChar("chWin", settings.chartWindow);
  prefs.putUShort("pollMs", settings.pollMs);
  prefs.putUChar("gapMs", settings.gapMs);
  prefs.end();
}
