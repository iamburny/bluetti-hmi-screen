#pragma once
#include <Arduino_GFX_Library.h>

// Shared UI palette (cohesive dark theme with one accent per function).
#define COL_BG RGB565(15, 17, 23)        // app background (near-black)
#define COL_BAR RGB565(26, 30, 40)       // dark backing (icon hubs, spinner disc)
#define COL_TILE RGB565(32, 37, 50)      // tile / key base
#define COL_TILE_DN RGB565(54, 62, 82)   // pressed
#define COL_TEXT RGB565(236, 239, 245)   // primary text
#define COL_MUTED RGB565(140, 150, 166)  // secondary text
#define COL_ON RGB565(74, 210, 126)      // ON pill / battery / confirm
#define COL_OFF RGB565(92, 100, 114)     // OFF pill
#define COL_WARN RGB565(240, 96, 96)     // cancel / errors

#define ACC_LIGHTS RGB565(255, 198, 73)   // amber
#define ACC_WEATHER RGB565(90, 178, 255)  // sky blue
#define ACC_LEVEL RGB565(60, 214, 184)    // teal
#define ACC_WATER RGB565(82, 138, 255)    // blue
#define ACC_SETTINGS RGB565(176, 156, 255)  // lilac
