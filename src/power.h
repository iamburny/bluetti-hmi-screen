#pragma once
#include <Arduino.h>

// Power telemetry for the Bluetti Elite 300, read natively over BLE and
// decrypted on-device (see bluetti.cpp / docs/BLUETTI.md). No bridge, no
// cloud, no licence file.

enum PowerStatus { PWR_NONE, PWR_FETCHING, PWR_OK, PWR_FAIL };

struct PowerData {
  PowerStatus status;
  int soc;       // battery state of charge, 0..100 %
  int dcInW;     // DC input (solar / DC charge), watts
  int acInW;     // AC input (grid / shore charge), watts
  int dcOutW;    // DC output (12V / USB loads), watts
  int acOutW;    // AC output (inverter loads), watts
  int whRemaining;  // remaining energy, watt-hours (0 = unknown)
  int ttfMin;    // minutes to empty (discharging) or full (charging); <=0 = n/a
  int tempC;     // battery temperature, deg C (reg 152)
  int acOutDV;   // AC output voltage, deci-volts (reg 1431; /10 = volts)
  int chargeMode;  // 0=Standard, 1=Silent, 2=Turbo, 4=Custom (reg 2020)
  int gridChargeA; // Custom max grid charging current, A (reg 2214)
  bool acEco;      // AC ECO enable (reg 2017)
  bool dcEco;      // DC ECO enable (reg 2014)
  bool powerLift;  // Power Lifting enable (reg 2021)
  int chargeLimit;   // charge ceiling %, high byte of reg 2083
  int screenTimeout; // screen-timeout enum (reg 2067: 2=30s,3=1m,4=5m,5=Never)
  int acOutFreqDHz;  // AC output frequency, deci-Hz (reg 1500; /10 = Hz)
  bool charging; // true when net charging
  bool acOn;     // AC output enabled (reg 2011)
  bool dcOn;     // DC output enabled (reg 2012)
  // BEST GUESS, unconfirmed by the vendor/community -- found by diffing a
  // register sweep idle vs. AC-charging (fan running); two candidates changed
  // together (a clean 0/1 flip and a 0/2 jump) and this is our pick between
  // them pending real-world confirmation. See docs/BLUETTI.md "Cooling fan /
  // grid-connected" section. Swap the two register numbers below if wrong.
  bool fanOn;          // cooling fan running (reg 103)
  bool gridConnected;  // AC/mains input detected, even before current flows (reg 161)
  uint32_t fetchedMs;  // millis() of last good reading
};

extern PowerData power;

// True when we have a fresh good reading (status OK and not stale).
bool power_valid();

// Last known SoC (0..100), or 0 when no valid reading.
int power_soc();
