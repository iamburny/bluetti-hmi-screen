#pragma once
#include <Arduino.h>

// Modal on-screen QWERTY keyboard. Call keyboard_open() to start editing, then
// route touch/draw through keyboard_*() while keyboard_active() is true.

// Begin editing. title shows above the field; initial pre-fills the buffer;
// maxLen caps length; mask=true renders dots (for passwords).
void keyboard_open(const char *title, const char *initial, size_t maxLen,
                   bool mask);

bool keyboard_active();

// Render the keyboard (full screen) and flush.
void keyboard_draw();

// Handle a tap. Returns true once the user commits (OK) or cancels; check
// keyboard_committed() for which. After return-true the keyboard is closed.
bool keyboard_handle_touch(int16_t x, int16_t y);

bool keyboard_committed();        // true = OK, false = cancelled
const char *keyboard_value();     // edited text (valid after commit)
