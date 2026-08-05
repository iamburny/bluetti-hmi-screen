// Host-side unit tests for the pure logic in include/logic/.
// Run with: pio test -e native
#include <unity.h>
#include "logic/touch_map.h"
#include "logic/geom.h"
#include "logic/modbus.h"
#include <string.h>

// Unity lifecycle hooks (required; no per-test setup needed here).
void setUp(void) {}
void tearDown(void) {}

// ---- touch frame parse -----------------------------------------------------

void test_parse_valid_single_touch() {
  // buf[0]=0 ok, buf[1]=1 (one point), x=0x123, y=0x045
  uint8_t buf[14] = {0};
  buf[1] = 1;
  buf[2] = 0x01; buf[3] = 0x23;  // x = 0x123 = 291
  buf[4] = 0x00; buf[5] = 0x45;  // y = 0x045 = 69
  int16_t rx = -1, ry = -1;
  TEST_ASSERT_TRUE(parseAxsTouch(buf, 14, rx, ry));
  TEST_ASSERT_EQUAL_INT16(0x123, rx);
  TEST_ASSERT_EQUAL_INT16(0x045, ry);
}

void test_parse_masks_high_nibble() {
  // Upper nibble of the coord bytes must be masked off (gesture/flags live there).
  uint8_t buf[14] = {0};
  buf[1] = 1;
  buf[2] = 0xF1; buf[3] = 0x23;  // -> 0x123, not 0xF123
  buf[4] = 0xA0; buf[5] = 0x45;  // -> 0x045
  int16_t rx = 0, ry = 0;
  TEST_ASSERT_TRUE(parseAxsTouch(buf, 14, rx, ry));
  TEST_ASSERT_EQUAL_INT16(0x123, rx);
  TEST_ASSERT_EQUAL_INT16(0x045, ry);
}

void test_parse_rejects_no_touch_and_short() {
  int16_t rx, ry;
  uint8_t none[14] = {0};       // count 0 -> invalid
  TEST_ASSERT_FALSE(parseAxsTouch(none, 14, rx, ry));
  uint8_t bad0[14] = {0};
  bad0[0] = 1; bad0[1] = 1;     // buf[0]!=0 -> invalid
  TEST_ASSERT_FALSE(parseAxsTouch(bad0, 14, rx, ry));
  uint8_t toomany[14] = {0};
  toomany[1] = 3;               // count >2 -> invalid
  TEST_ASSERT_FALSE(parseAxsTouch(toomany, 14, rx, ry));
  uint8_t shortb[4] = {0, 1, 0, 0};
  TEST_ASSERT_FALSE(parseAxsTouch(shortb, 4, rx, ry));  // len<6
}

// ---- rotation transform (panel native 320x480) -----------------------------

void test_map_rotation0_identity() {
  int16_t x, y;
  mapTouch(50, 100, 0, 320, 480, 320, 480, x, y);
  TEST_ASSERT_EQUAL_INT16(50, x);
  TEST_ASSERT_EQUAL_INT16(100, y);
}

void test_map_rotation1_landscape() {
  // rotation 1: x=rawY, y=(panelW-1)-rawX; screen is 480x320.
  int16_t x, y;
  mapTouch(0, 0, 1, 320, 480, 480, 320, x, y);
  TEST_ASSERT_EQUAL_INT16(0, x);
  TEST_ASSERT_EQUAL_INT16(319, y);          // (320-1) - 0
  mapTouch(319, 479, 1, 320, 480, 480, 320, x, y);
  TEST_ASSERT_EQUAL_INT16(479, x);          // rawY
  TEST_ASSERT_EQUAL_INT16(0, y);            // (320-1) - 319
}

void test_map_clamps_out_of_range() {
  int16_t x, y;
  mapTouch(9999, 9999, 0, 320, 480, 320, 480, x, y);
  TEST_ASSERT_EQUAL_INT16(319, x);          // clamped to screenW-1
  TEST_ASSERT_EQUAL_INT16(479, y);          // clamped to screenH-1
}

// ---- hit testing -----------------------------------------------------------

void test_inrect() {
  TEST_ASSERT_TRUE(inRect(15, 25, 10, 20, 100, 50));
  TEST_ASSERT_TRUE(inRect(10, 20, 10, 20, 100, 50));   // top-left inclusive
  TEST_ASSERT_FALSE(inRect(110, 25, 10, 20, 100, 50)); // right edge exclusive
  TEST_ASSERT_FALSE(inRect(15, 70, 10, 20, 100, 50));  // bottom edge exclusive
  TEST_ASSERT_FALSE(inRect(5, 25, 10, 20, 100, 50));   // left of rect
}

// ---- Modbus framing / parsing (Bluetti BLE) --------------------------------

void test_modbus_crc_known() {
  // Standard FC3 request 01 03 00 00 00 01 has on-wire CRC 84 0A.
  uint8_t f[6] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x01};
  TEST_ASSERT_EQUAL_HEX16(0x0A84, modbus_crc16(f, 6));
}

void test_modbus_build_read() {
  uint8_t f[8];
  modbus_build_read(f, 102, 1);  // SoC register
  TEST_ASSERT_EQUAL_HEX8(0x01, f[0]);
  TEST_ASSERT_EQUAL_HEX8(0x03, f[1]);
  TEST_ASSERT_EQUAL_HEX8(0x00, f[2]);
  TEST_ASSERT_EQUAL_HEX8(0x66, f[3]);  // 102
  TEST_ASSERT_EQUAL_HEX8(0x00, f[4]);
  TEST_ASSERT_EQUAL_HEX8(0x01, f[5]);
  uint16_t crc = modbus_crc16(f, 6);
  TEST_ASSERT_EQUAL_HEX8(crc & 0xFF, f[6]);  // CRC little-endian on the wire
  TEST_ASSERT_EQUAL_HEX8(crc >> 8, f[7]);
}

void test_modbus_build_write() {
  uint8_t f[8];
  modbus_build_write(f, 2011, 1);  // CTRL_AC = 0x07DB
  TEST_ASSERT_EQUAL_HEX8(0x06, f[1]);
  TEST_ASSERT_EQUAL_HEX8(0x07, f[2]);
  TEST_ASSERT_EQUAL_HEX8(0xDB, f[3]);
  TEST_ASSERT_EQUAL_HEX8(0x00, f[4]);
  TEST_ASSERT_EQUAL_HEX8(0x01, f[5]);
  TEST_ASSERT_TRUE(modbus_is_write_echo(f, 8));  // device echoes the request
}

// Build a valid FC3 response for `qty` regs from `vals` into r[]; returns length.
static int build_read_resp(uint8_t *r, const uint16_t *vals, int qty) {
  r[0] = 0x01;
  r[1] = 0x03;
  r[2] = (uint8_t)(qty * 2);
  for (int i = 0; i < qty; i++) {
    r[3 + i * 2] = (uint8_t)(vals[i] >> 8);
    r[4 + i * 2] = (uint8_t)(vals[i] & 0xFF);
  }
  uint16_t crc = modbus_crc16(r, 3 + qty * 2);
  r[3 + qty * 2] = (uint8_t)(crc & 0xFF);
  r[4 + qty * 2] = (uint8_t)(crc >> 8);
  return 5 + qty * 2;
}

void test_modbus_parse_read_valid() {
  // The 140-block: DC_out, -, AC_out, -, DC_in, -, AC_in, -.
  const uint16_t vals[8] = {25, 0, 31, 0, 0, 0, 0, 0};
  uint8_t r[21];
  int len = build_read_resp(r, vals, 8);
  uint16_t w[8] = {0};
  TEST_ASSERT_EQUAL_INT(8, modbus_parse_read(r, len, 8, w, 8));
  TEST_ASSERT_EQUAL_UINT16(25, w[0]);  // DC out
  TEST_ASSERT_EQUAL_UINT16(31, w[2]);  // AC out
  TEST_ASSERT_EQUAL_UINT16(0, w[4]);   // DC in
}

void test_modbus_parse_read_rejects() {
  const uint16_t vals[1] = {94};
  uint8_t r[7];
  int len = build_read_resp(r, vals, 1);
  uint16_t w[8];
  TEST_ASSERT_EQUAL_INT(1, modbus_parse_read(r, len, 1, w, 8));  // sanity
  TEST_ASSERT_EQUAL_UINT16(94, w[0]);

  TEST_ASSERT_EQUAL_INT(-1, modbus_parse_read(r, len, 4, w, 8));  // qty mismatch
  r[len - 1] ^= 0xFF;  // corrupt CRC
  TEST_ASSERT_EQUAL_INT(-1, modbus_parse_read(r, len, 1, w, 8));
  r[len - 1] ^= 0xFF;
  TEST_ASSERT_EQUAL_INT(-1, modbus_parse_read(r, 3, 1, w, 8));  // too short

  // MODBUS exception (func 0x83) must be rejected, not parsed.
  uint8_t exc[5] = {0x01, 0x83, 0x02, 0, 0};
  uint16_t ec = modbus_crc16(exc, 3);
  exc[3] = ec & 0xFF;
  exc[4] = ec >> 8;
  TEST_ASSERT_EQUAL_INT(-1, modbus_parse_read(exc, 5, 1, w, 8));
}

void test_modbus_write_echo_rejects() {
  uint8_t e[8];
  modbus_build_write(e, 2012, 0);
  e[7] ^= 0xFF;  // corrupt CRC
  TEST_ASSERT_FALSE(modbus_is_write_echo(e, 8));
  uint8_t r[8];
  modbus_build_read(r, 100, 1);  // FC3 frame is not a write echo
  TEST_ASSERT_FALSE(modbus_is_write_echo(r, 8));
}

void test_bluetti_helpers() {
  uint8_t body[6] = {0x02, 0x04, 0x10, 0x20, 0x30, 0x40};
  TEST_ASSERT_EQUAL_HEX16(0x00A6, bluetti_kex_sum16(body, 6));  // 166
  TEST_ASSERT_EQUAL_INT(0, bluetti_clamp_soc(-5));
  TEST_ASSERT_EQUAL_INT(100, bluetti_clamp_soc(150));
  TEST_ASSERT_EQUAL_INT(73, bluetti_clamp_soc(73));
}

int main(int, char **) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_valid_single_touch);
  RUN_TEST(test_parse_masks_high_nibble);
  RUN_TEST(test_parse_rejects_no_touch_and_short);
  RUN_TEST(test_map_rotation0_identity);
  RUN_TEST(test_map_rotation1_landscape);
  RUN_TEST(test_map_clamps_out_of_range);
  RUN_TEST(test_inrect);
  RUN_TEST(test_modbus_crc_known);
  RUN_TEST(test_modbus_build_read);
  RUN_TEST(test_modbus_build_write);
  RUN_TEST(test_modbus_parse_read_valid);
  RUN_TEST(test_modbus_parse_read_rejects);
  RUN_TEST(test_modbus_write_echo_rejects);
  RUN_TEST(test_bluetti_helpers);
  return UNITY_END();
}
