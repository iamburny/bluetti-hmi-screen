// Pure Modbus-RTU + Bluetti framing helpers (hardware-free; unit-testable and
// shared with the firmware's BLE client). No Arduino/mbedTLS deps.
#pragma once
#include <stdint.h>
#include <stddef.h>

// Modbus RTU CRC16 (poly 0xA001, init 0xFFFF).
static inline uint16_t modbus_crc16(const uint8_t* d, size_t n) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < n; i++) {
    crc ^= d[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (uint16_t)((crc >> 1) ^ 0xA001) : (uint16_t)(crc >> 1);
  }
  return crc;
}

// Build an 8-byte FC3 "read holding registers" request into out[8].
static inline void modbus_build_read(uint8_t out[8], uint16_t addr,
                                     uint16_t qty) {
  out[0] = 0x01;  // slave address
  out[1] = 0x03;  // function code
  out[2] = (uint8_t)(addr >> 8);
  out[3] = (uint8_t)(addr & 0xFF);
  out[4] = (uint8_t)(qty >> 8);
  out[5] = (uint8_t)(qty & 0xFF);
  uint16_t crc = modbus_crc16(out, 6);
  out[6] = (uint8_t)(crc & 0xFF);  // CRC is little-endian on the wire
  out[7] = (uint8_t)(crc >> 8);
}

// Build an 8-byte FC6 "write single register" request into out[8].
static inline void modbus_build_write(uint8_t out[8], uint16_t addr,
                                      uint16_t val) {
  out[0] = 0x01;
  out[1] = 0x06;
  out[2] = (uint8_t)(addr >> 8);
  out[3] = (uint8_t)(addr & 0xFF);
  out[4] = (uint8_t)(val >> 8);
  out[5] = (uint8_t)(val & 0xFF);
  uint16_t crc = modbus_crc16(out, 6);
  out[6] = (uint8_t)(crc & 0xFF);
  out[7] = (uint8_t)(crc >> 8);
}

// Parse an FC3 response: validate slave/function/byte-count/CRC and that it
// answers a `qty`-register request, then extract big-endian 16-bit words into
// words[]. Returns the word count written (<= maxw), or -1 on any mismatch.
static inline int modbus_parse_read(const uint8_t* r, size_t len, uint16_t qty,
                                    uint16_t* words, int maxw) {
  if (len < 5) return -1;
  if (r[0] != 0x01 || r[1] != 0x03) return -1;  // (also rejects 0x83 exceptions)
  int bc = r[2];
  if (bc != (int)qty * 2) return -1;            // not the answer to THIS request
  if ((int)len < 3 + bc + 2) return -1;
  uint16_t crc = modbus_crc16(r, (size_t)(3 + bc));
  if (r[3 + bc] != (uint8_t)(crc & 0xFF) ||
      r[3 + bc + 1] != (uint8_t)(crc >> 8))
    return -1;
  int n = bc / 2;
  for (int i = 0; i < n && i < maxw; i++)
    words[i] = (uint16_t)((r[3 + i * 2] << 8) | r[4 + i * 2]);
  return n;
}

// Validate an FC6 write echo (slave 0x01, function 0x06, correct CRC).
static inline bool modbus_is_write_echo(const uint8_t* r, size_t len) {
  if (len < 8) return false;
  if (r[0] != 0x01 || r[1] != 0x06) return false;
  uint16_t crc = modbus_crc16(r, 6);
  return r[6] == (uint8_t)(crc & 0xFF) && r[7] == (uint8_t)(crc >> 8);
}

// Bluetti pre-key-exchange frame checksum: low 16 bits of the byte sum.
static inline uint16_t bluetti_kex_sum16(const uint8_t* body, size_t n) {
  uint32_t s = 0;
  for (size_t i = 0; i < n; i++) s += body[i];
  return (uint16_t)(s & 0xFFFF);
}

// Clamp a raw state-of-charge reading to 0..100.
static inline int bluetti_clamp_soc(int v) {
  return v < 0 ? 0 : (v > 100 ? 100 : v);
}
