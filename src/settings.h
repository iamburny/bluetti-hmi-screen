#pragma once
#include <Arduino.h>

// Persisted device configuration (stored in NVS via Preferences).
struct Settings {
  char bluettiMac[18];  // Elite 300 BLE MAC "aa:bb:..", "" = auto-discover by name
  uint8_t chartSeriesMask;  // power-chart trace visibility, bit0..4 (default 0x1F)
  uint8_t chartWindow;      // power-chart X span: 0=1h, 1=6h, 2=24h
};

extern Settings settings;

// Load settings from flash (applies defaults on first boot).
void settings_load();

// Persist the current settings to flash.
void settings_save();
