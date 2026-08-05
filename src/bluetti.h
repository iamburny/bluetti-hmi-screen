#pragma once
#include <cstdint>

// Native BLE client for the Bluetti Elite 300. Runs a background task that
// periodically connects, performs the encrypted v2 handshake (bluetti_crypt),
// reads the telemetry registers, fills the shared `power` struct (power.h), and
// disconnects. No bridge, no licence — decryption happens on-device.

// Initialise NimBLE and start the poller task. Call once in setup().
void bluetti_begin();

// Request an AC (ac=true) or DC (ac=false) output on/off change. Queued and
// applied by the BLE task on its next (immediately-woken) connection.
void bluetti_set_output(bool ac, bool on);

// Queue a raw Modbus register write (FC6), applied on the next (woken) cycle.
void bluetti_write_reg(uint16_t addr, uint16_t val);

// Release the BLE link for `seconds` so the Bluetti phone app can connect; the
// HMI disconnects and stops polling until it expires. Call with 0 to resume now.
void bluetti_release(uint32_t seconds);

// Seconds remaining in the released ("app") window, or 0 if active/polling.
uint32_t bluetti_release_remaining();

// BLE link state for the status-bar indicator.
enum BluettiConn { BTC_OFFLINE = 0, BTC_CONNECTING, BTC_ONLINE };
BluettiConn bluetti_state();

// ===== Diagnostics: live register change detection =========================
// Built to hunt the cooling-fan register, which two offline sweep campaigns
// failed to find (see docs/BLUETTI.md). While enabled, the BLE task sweeps
// every known-readable register after each poll and records any that CHANGED
// against a stored baseline, so a thermally-triggered event can be caught the
// instant it happens instead of hoping a serial capture window straddles it.
// Registers already characterised as continuously-drifting (watts, volts,
// counters) are filtered out so the list stays readable.
//
// Only enable while the Diagnostics page is open -- a full sweep is ~940
// registers (~6 s), which badly slows the normal telemetry refresh.

struct RegChange {
  uint16_t addr;
  uint16_t oldVal;
  uint16_t newVal;
  uint32_t atMs;  // millis() when the change was observed
};

void bluetti_diag_enable(bool on);
bool bluetti_diag_enabled();

// Drop the baseline and the recorded changes; the next sweep re-baselines.
void bluetti_diag_reset();

// Recorded changes, newest first (0 = most recent). Capped at a small ring.
int bluetti_diag_count();
const RegChange &bluetti_diag_at(int i);

// Last completed sweep: duration and how many registers answered.
uint32_t bluetti_diag_scan_ms();
int bluetti_diag_scanned();
