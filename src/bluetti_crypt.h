#pragma once
#include <Arduino.h>
#include <vector>
#include <mbedtls/ecp.h>
#include <mbedtls/bignum.h>

// On-device port of the open-source Bluetti "v2" BLE handshake
// (github.com/Patrick762/bluetti-bt-lib bluetti_bt_lib/bluetooth/encryption.py),
// using the ESP32's bundled mbedTLS. Uses hardcoded universal keys — no licence.
//
// Handshake (driven by handle() on each notification):
//   device CHALLENGE (2a2a,t1)      -> reply (write to ff02)         [returns 1]
//   device CHALLENGE_ACCEPTED (t3)  -> ignore                        [returns 0]
//   device PEER_PUBKEY (enc, t4)    -> verify+reply (ECDH setup)     [returns 1]
//   device PUBKEY_ACCEPTED (enc,t6) -> derive secure key            [returns 2]
//   any data message (enc)          -> decrypted modbus in `plain`   [returns 3]
// returns -1 on error.
class BluettiCrypt {
 public:
  BluettiCrypt();
  ~BluettiCrypt();

  void reset();
  bool isReady() const { return ready_; }

  int handle(const uint8_t* data, size_t len, std::vector<uint8_t>& out,
             std::vector<uint8_t>& plain);

  // Build an encrypted Modbus FC3 "read holding registers" command for ff02.
  std::vector<uint8_t> readCommand(uint16_t addr, uint16_t qty);

  // Build an encrypted Modbus FC6 "write single register" command for ff02.
  std::vector<uint8_t> writeCommand(uint16_t addr, uint16_t val);

 private:
  bool unsecureSet_ = false;
  bool ready_ = false;
  uint8_t unsecKey_[16];
  uint8_t unsecIv_[16];
  uint8_t secKey_[32];

  mbedtls_ecp_group grp_;
  mbedtls_mpi myD_;
  mbedtls_ecp_point myQ_;
  mbedtls_ecp_point peerQ_;

  std::vector<uint8_t> aesDecrypt(const uint8_t* data, size_t len,
                                  const uint8_t* key, size_t keylen,
                                  const uint8_t* iv);
  std::vector<uint8_t> aesEncrypt(const std::vector<uint8_t>& data,
                                  const uint8_t* key, size_t keylen,
                                  const uint8_t* iv);
  std::vector<uint8_t> onChallenge(const uint8_t* d4);
  std::vector<uint8_t> onPeerPubkey(const uint8_t* data128);
  bool onPubkeyAccepted(const uint8_t* d, size_t n);
};
