#include "bluetti_crypt.h"
#include "logic/modbus.h"
#include <esp_random.h>
#include <string.h>
#include <mbedtls/aes.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>
#include <mbedtls/ecdsa.h>
#include <mbedtls/ecdh.h>

// ---- universal keys (from encryption.py) -----------------------------------
static const uint8_t LOCAL_AES_KEY[16] = {
    0x45, 0x9F, 0xC5, 0x35, 0x80, 0x89, 0x41, 0xF1,
    0x70, 0x91, 0xE0, 0x99, 0x3E, 0xE3, 0xE9, 0x3D};
static const uint8_t PRIVATE_KEY_L1[32] = {
    0x4F, 0x19, 0xA1, 0x6E, 0x3E, 0x87, 0xBD, 0xD9, 0xBD, 0x24, 0xD3,
    0xE5, 0x49, 0x5B, 0x88, 0x04, 0x15, 0x11, 0x94, 0x3C, 0xBC, 0x8B,
    0x96, 0x9A, 0xDE, 0x96, 0x41, 0xD0, 0xF5, 0x6A, 0xF3, 0x37};
// PUBLIC_KEY_K2 uncompressed point X||Y (the bytes after the DER prefix ...0004)
static const uint8_t PUBLIC_KEY_K2_XY[64] = {
    0xA7, 0x3A, 0xBF, 0x5D, 0x22, 0x32, 0xC8, 0xC1, 0xC7, 0x2E, 0x68,
    0x30, 0x43, 0x43, 0xC2, 0x72, 0x49, 0x5E, 0x3A, 0x8F, 0xD6, 0xF3,
    0x0E, 0xA9, 0x6D, 0xE2, 0xF4, 0xB3, 0xCE, 0x60, 0xB2, 0x51, 0xEE,
    0x21, 0xAC, 0x66, 0x7C, 0xF8, 0xA7, 0x1E, 0x18, 0xB4, 0x6B, 0x66,
    0x4E, 0xAE, 0xFF, 0xE3, 0xC4, 0x89, 0xF2, 0x4F, 0x69, 0x5B, 0x64,
    0x11, 0xDB, 0x7E, 0x22, 0xCC, 0xC8, 0x5A, 0x85, 0x94};

static int rng(void*, unsigned char* p, size_t n) {
  esp_fill_random(p, n);
  return 0;
}

static void md5(const uint8_t* in, size_t n, uint8_t out[16]) {
  mbedtls_md5(in, n, out);
}
static void sha256(const uint8_t* in, size_t n, uint8_t out[32]) {
  mbedtls_sha256(in, n, out, 0);
}

// 2-byte big-endian Bluetti kex checksum (low 16 bits of the byte sum).
static void appendSum16(std::vector<uint8_t>& v, const uint8_t* body, size_t n) {
  uint16_t s = bluetti_kex_sum16(body, n);
  v.push_back((s >> 8) & 0xFF);
  v.push_back(s & 0xFF);
}

BluettiCrypt::BluettiCrypt() {
  mbedtls_ecp_group_init(&grp_);
  mbedtls_ecp_group_load(&grp_, MBEDTLS_ECP_DP_SECP256R1);
  mbedtls_mpi_init(&myD_);
  mbedtls_ecp_point_init(&myQ_);
  mbedtls_ecp_point_init(&peerQ_);
}

BluettiCrypt::~BluettiCrypt() {
  mbedtls_ecp_group_free(&grp_);
  mbedtls_mpi_free(&myD_);
  mbedtls_ecp_point_free(&myQ_);
  mbedtls_ecp_point_free(&peerQ_);
}

void BluettiCrypt::reset() {
  unsecureSet_ = false;
  ready_ = false;
  mbedtls_mpi_free(&myD_);
  mbedtls_ecp_point_free(&myQ_);
  mbedtls_ecp_point_free(&peerQ_);
  mbedtls_mpi_init(&myD_);
  mbedtls_ecp_point_init(&myQ_);
  mbedtls_ecp_point_init(&peerQ_);
}

std::vector<uint8_t> BluettiCrypt::aesDecrypt(const uint8_t* data, size_t len,
                                              const uint8_t* key, size_t keylen,
                                              const uint8_t* iv) {
  if (len < 6) return {};
  size_t data_len = (size_t)(data[0] << 8) | data[1];
  uint8_t ivbuf[16];
  const uint8_t* enc;
  size_t enclen;
  if (iv == nullptr) {
    md5(data + 2, 4, ivbuf);
    enc = data + 6;
    enclen = len - 6;
  } else {
    memcpy(ivbuf, iv, 16);
    enc = data + 2;
    enclen = len - 2;
  }
  if (enclen == 0 || enclen % 16 != 0) return {};
  std::vector<uint8_t> out(enclen);
  mbedtls_aes_context a;
  mbedtls_aes_init(&a);
  mbedtls_aes_setkey_dec(&a, key, keylen * 8);
  int rc = mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_DECRYPT, enclen, ivbuf, enc,
                                 out.data());
  mbedtls_aes_free(&a);
  if (rc != 0) return {};
  if (data_len > enclen) data_len = enclen;
  out.resize(data_len);
  return out;
}

std::vector<uint8_t> BluettiCrypt::aesEncrypt(const std::vector<uint8_t>& data,
                                              const uint8_t* key, size_t keylen,
                                              const uint8_t* iv) {
  std::vector<uint8_t> out;
  out.push_back((data.size() >> 8) & 0xFF);
  out.push_back(data.size() & 0xFF);
  uint8_t ivbuf[16];
  if (iv == nullptr) {
    uint8_t seed[4];
    esp_fill_random(seed, 4);
    md5(seed, 4, ivbuf);
    out.insert(out.end(), seed, seed + 4);
  } else {
    memcpy(ivbuf, iv, 16);
  }
  std::vector<uint8_t> buf = data;
  size_t pad = (16 - buf.size() % 16) % 16;
  buf.resize(buf.size() + pad, 0);
  std::vector<uint8_t> ct(buf.size());
  mbedtls_aes_context a;
  mbedtls_aes_init(&a);
  mbedtls_aes_setkey_enc(&a, key, keylen * 8);
  int rc = mbedtls_aes_crypt_cbc(&a, MBEDTLS_AES_ENCRYPT, buf.size(), ivbuf,
                                 buf.data(), ct.data());
  mbedtls_aes_free(&a);
  if (rc != 0) return {};
  out.insert(out.end(), ct.begin(), ct.end());
  return out;
}

std::vector<uint8_t> BluettiCrypt::onChallenge(const uint8_t* d4) {
  uint8_t rev[4] = {d4[3], d4[2], d4[1], d4[0]};
  md5(rev, 4, unsecIv_);
  for (int i = 0; i < 16; i++) unsecKey_[i] = unsecIv_[i] ^ LOCAL_AES_KEY[i];
  unsecureSet_ = true;
  uint8_t body[6] = {0x02, 0x04, unsecIv_[8], unsecIv_[9], unsecIv_[10],
                     unsecIv_[11]};
  std::vector<uint8_t> msg = {0x2a, 0x2a};
  msg.insert(msg.end(), body, body + 6);
  appendSum16(msg, body, 6);
  return msg;
}

std::vector<uint8_t> BluettiCrypt::onPeerPubkey(const uint8_t* data128) {
  // 1) verify the peer's signed pubkey against the well-known K2 key.
  uint8_t signed_data[80];
  memcpy(signed_data, data128, 64);
  memcpy(signed_data + 64, unsecIv_, 16);
  uint8_t hash[32];
  sha256(signed_data, 80, hash);

  mbedtls_ecp_point K2;
  mbedtls_ecp_point_init(&K2);
  uint8_t k2pt[65];
  k2pt[0] = 0x04;
  memcpy(k2pt + 1, PUBLIC_KEY_K2_XY, 64);
  mbedtls_mpi r, s;
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  std::vector<uint8_t> out;
  bool ok = false;
  do {
    if (mbedtls_ecp_point_read_binary(&grp_, &K2, k2pt, 65) != 0) break;
    if (mbedtls_mpi_read_binary(&r, data128 + 64, 32) != 0) break;
    if (mbedtls_mpi_read_binary(&s, data128 + 96, 32) != 0) break;
    if (mbedtls_ecdsa_verify(&grp_, hash, 32, &K2, &r, &s) != 0) break;

    // 2) load the peer pubkey point for ECDH (04 || X || Y).
    uint8_t peerpt[65];
    peerpt[0] = 0x04;
    memcpy(peerpt + 1, data128, 64);
    if (mbedtls_ecp_point_read_binary(&grp_, &peerQ_, peerpt, 65) != 0) break;

    // 3) generate our ephemeral keypair.
    if (mbedtls_ecp_gen_keypair(&grp_, &myD_, &myQ_, rng, nullptr) != 0) break;
    uint8_t mypub[65];
    size_t olen = 0;
    if (mbedtls_ecp_point_write_binary(&grp_, &myQ_,
                                       MBEDTLS_ECP_PF_UNCOMPRESSED, &olen, mypub,
                                       sizeof(mypub)) != 0 ||
        olen != 65)
      break;

    // 4) sign our pubkey (+iv) with the well-known L1 key.
    uint8_t tosign[80];
    memcpy(tosign, mypub + 1, 64);
    memcpy(tosign + 64, unsecIv_, 16);
    uint8_t hash2[32];
    sha256(tosign, 80, hash2);
    mbedtls_mpi dL1, sr, ss;
    mbedtls_mpi_init(&dL1);
    mbedtls_mpi_init(&sr);
    mbedtls_mpi_init(&ss);
    uint8_t rawsig[64];
    bool sig_ok = false;
    if (mbedtls_mpi_read_binary(&dL1, PRIVATE_KEY_L1, 32) == 0 &&
        mbedtls_ecdsa_sign(&grp_, &sr, &ss, &dL1, hash2, 32, rng, nullptr) == 0 &&
        mbedtls_mpi_write_binary(&sr, rawsig, 32) == 0 &&
        mbedtls_mpi_write_binary(&ss, rawsig + 32, 32) == 0) {
      sig_ok = true;
    }
    mbedtls_mpi_free(&dL1);
    mbedtls_mpi_free(&sr);
    mbedtls_mpi_free(&ss);
    if (!sig_ok) break;

    // 5) build body 0580 + mypub(64) + rawsig(64), wrap "**"+body+sum, encrypt.
    std::vector<uint8_t> body;
    body.push_back(0x05);
    body.push_back(0x80);
    body.insert(body.end(), mypub + 1, mypub + 65);
    body.insert(body.end(), rawsig, rawsig + 64);
    std::vector<uint8_t> msg = {0x2a, 0x2a};
    msg.insert(msg.end(), body.begin(), body.end());
    appendSum16(msg, body.data(), body.size());
    out = aesEncrypt(msg, unsecKey_, 16, unsecIv_);
    ok = !out.empty();
  } while (false);

  mbedtls_ecp_point_free(&K2);
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&s);
  if (!ok) return {};
  return out;
}

bool BluettiCrypt::onPubkeyAccepted(const uint8_t* d, size_t n) {
  if (n < 1 || d[0] != 0) return false;
  mbedtls_mpi z;
  mbedtls_mpi_init(&z);
  bool ok = false;
  if (mbedtls_ecdh_compute_shared(&grp_, &z, &peerQ_, &myD_, rng, nullptr) == 0 &&
      mbedtls_mpi_write_binary(&z, secKey_, 32) == 0) {
    ready_ = true;
    ok = true;
  }
  mbedtls_mpi_free(&z);
  return ok;
}

int BluettiCrypt::handle(const uint8_t* data, size_t len,
                         std::vector<uint8_t>& out, std::vector<uint8_t>& plain) {
  if (len < 4) return -1;
  // Cleartext pre-key-exchange frames (challenge / challenge-accepted).
  if (data[0] == 0x2a && data[1] == 0x2a) {
    uint8_t type = data[2];
    if (type == 1) {  // CHALLENGE: data = body[2:] = 4 bytes
      if (len < 4 + 4 + 2) return -1;
      out = onChallenge(data + 4);
      return out.empty() ? -1 : 1;
    }
    if (type == 3) return 0;  // CHALLENGE_ACCEPTED
  }
  if (!unsecureSet_) return -1;

  const uint8_t* key = ready_ ? secKey_ : unsecKey_;
  size_t keylen = ready_ ? 32 : 16;
  const uint8_t* iv = ready_ ? nullptr : unsecIv_;
  std::vector<uint8_t> dec = aesDecrypt(data, len, key, keylen, iv);
  if (dec.empty()) return -1;

  if (dec.size() >= 6 && dec[0] == 0x2a && dec[1] == 0x2a) {
    uint8_t type = dec[2];
    const uint8_t* payload = dec.data() + 4;
    size_t plen = dec.size() - 4 - 2;
    if (type == 4) {  // PEER_PUBKEY (128-byte payload)
      if (plen < 128) return -1;
      out = onPeerPubkey(payload);
      return out.empty() ? -1 : 1;
    }
    if (type == 6) {  // PUBKEY_ACCEPTED
      return onPubkeyAccepted(payload, plen) ? 2 : -1;
    }
    return 0;
  }

  plain = dec;  // decrypted Modbus response
  return 3;
}

std::vector<uint8_t> BluettiCrypt::readCommand(uint16_t addr, uint16_t qty) {
  uint8_t cmd[8];
  modbus_build_read(cmd, addr, qty);
  std::vector<uint8_t> v(cmd, cmd + 8);
  return aesEncrypt(v, secKey_, 32, nullptr);
}

std::vector<uint8_t> BluettiCrypt::writeCommand(uint16_t addr, uint16_t val) {
  uint8_t cmd[8];
  modbus_build_write(cmd, addr, val);
  std::vector<uint8_t> v(cmd, cmd + 8);
  return aesEncrypt(v, secKey_, 32, nullptr);
}
