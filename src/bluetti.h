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
