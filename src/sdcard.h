#pragma once

// Shared microSD mount (SD_MMC 1-bit: CLK=12, CMD=11, D0=13). Mounts once;
// idempotent, safe to call from multiple modules. Returns true if mounted.
bool sd_begin();
