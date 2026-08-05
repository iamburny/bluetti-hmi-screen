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

// ===== Link health + poll-rate tuning ======================================
// The refresh rate is limited by how long a poll TAKES (~17 sequential reads,
// each a 40ms gap plus a notify round-trip), not by the sleep between polls --
// set the interval below the poll duration and it simply polls back-to-back.
// These counters exist so the interval can be dialled down against real
// failure data rather than guesswork: retries/failures/drops climbing is the
// signal that the link is being pushed too hard.

struct BluettiStats {
  uint32_t lastPollMs;    // duration of the most recent completed poll
  uint32_t avgPollMs;     // rolling mean poll duration
  uint16_t lastReads;     // register reads attempted in that poll
  uint16_t lastRetries;   // of those, ones that needed a retry
  uint16_t lastFailures;  // of those, ones that gave up entirely
  uint32_t polls;         // completed polls since boot
  uint32_t retries;       // lifetime retried reads
  uint32_t failures;      // lifetime failed reads
  uint32_t linkDrops;     // lifetime forced reconnects
};
const BluettiStats &bluetti_stats();

// Zero the lifetime counters (so a new interval can be judged on its own).
void bluetti_stats_reset();

// Live poll interval, ms. Clamped to a sane range; persisted by the caller.
void bluetti_set_poll_ms(uint32_t ms);
uint32_t bluetti_poll_ms();

// Inter-command gap, ms (default 40). This is the DOMINANT cost of a poll --
// 40ms x 17 reads = 680ms of a measured ~1020-1320ms cycle -- so lowering it
// is the only way under the ~1.1s floor without cutting reads. The device is
// unreliable with back-to-back commands, so drop it gradually and watch the
// retry/failure counters. Writes always use the full default regardless.
void bluetti_set_gap_ms(uint32_t ms);
uint32_t bluetti_gap_ms();

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
