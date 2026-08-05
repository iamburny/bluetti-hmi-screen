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
  settings.chartSeriesMask = prefs.getUChar("chMask", 0x1F);  // all 5 traces on
  settings.chartWindow = prefs.getUChar("chWin", 0);          // default 1h
  prefs.end();
}

void settings_save() {
  prefs.begin(NS, false);  // read-write
  prefs.putString("btmac", settings.bluettiMac);
  prefs.putUChar("chMask", settings.chartSeriesMask);
  prefs.putUChar("chWin", settings.chartWindow);
  prefs.end();
}
