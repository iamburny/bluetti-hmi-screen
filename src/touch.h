#pragma once
#include <Arduino.h>

// Initialize the 0x3B capacitive touch controller.
void touch_init();

// If the panel is being touched, write the first point's screen coordinates
// into x/y and return true; otherwise return false.
bool touch_get(int16_t &x, int16_t &y);
