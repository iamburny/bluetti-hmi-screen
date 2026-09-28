// Layout of the Bluetti peer-pubkey signature field (hardware-free;
// unit-testable + shared with firmware).
#pragma once
#include <stdint.h>

// The device sends ECDSA r and s in a fixed 64-byte field, but not always as a
// plain 32+32 split: when r or s would start with 0x00 it is sent as 31 bytes,
// and a 0x00 pad fills the last byte. Which of the two was shortened isn't
// signalled, so a verifier tries each candidate split until one verifies.
struct BluettiSigSplit {
  uint8_t rLen;  // bytes of r, starting at offset 0
  uint8_t sOff;  // offset of s
  uint8_t sLen;  // bytes of s
};

// Fills out[] with the candidate splits of a 64-byte signature field, most
// likely first. Returns how many (1 when the last byte isn't a pad, else 3).
static inline int bluetti_sig_splits(const uint8_t sig[64], BluettiSigSplit out[3]) {
  out[0] = {32, 32, 32};  // plain r || s
  if (sig[63] != 0) return 1;
  out[1] = {31, 31, 32};  // r shortened to 31 bytes
  out[2] = {32, 32, 31};  // s shortened to 31 bytes
  return 3;
}
