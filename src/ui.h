#pragma once
#include <Arduino.h>

// Initialize UI state and draw the initial (Power) screen.
void ui_init();

// Render the current screen into the canvas and flush to the panel.
void ui_draw();

// Handle a single tap at screen coords (x, y). Redraws internally on change.
void ui_handle_touch(int16_t x, int16_t y);

// Called every frame the finger stays down (continuous controls). No-op here
// — kept for main.cpp's touch state machine, which calls it unconditionally.
void ui_handle_drag(int16_t x, int16_t y);

// Called when a touch lifts, with the press-start and release coords — used
// for the swipe-back gesture on sub-screens.
void ui_handle_release(int16_t x0, int16_t y0, int16_t x1, int16_t y1);

// Call frequently from loop(); refreshes time/data-driven elements (status
// bar, pending-write spinner, live chart data).
void ui_tick();
