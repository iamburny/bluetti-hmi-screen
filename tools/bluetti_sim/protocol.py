"""Device side of the Bluetti "v2" encrypted BLE protocol.

Mirrors the client in src/bluetti_crypt.cpp from the other end: the HMI
subscribes, we send a challenge, run the key exchange, then answer encrypted
Modbus FC3 reads and FC6 writes out of a Registers model. No BLE in here --
ble_server.py moves the bytes -- so the whole exchange is unit-testable.

The real unit signs its key with a Bluetti-only key (the HMI verifies it with
K2). We sign with the app key L1 instead, which only the BLUETTI_SIM firmware
build accepts.
"""
import hashlib
import os

from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import (
    decode_dss_signature, encode_dss_signature)
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

LOCAL_AES_KEY = bytes.fromhex("459FC535808941F17091E0993EE3E93D")
PRIVATE_KEY_L1 = ec.derive_private_key(
    int("4F19A16E3E87BDD9BD24D3E5495B88041511943CBC8B969ADE9641D0F56AF337", 16),
    ec.SECP256R1())
PUBLIC_KEY_L1 = PRIVATE_KEY_L1.public_key()


# ---- shared helpers (also used by the test's client port) ------------------
def md5(b):
    return hashlib.md5(b).digest()


def sum16(body):
    return (sum(body) & 0xFFFF).to_bytes(2, "big")


def kex_frame(body):
    """ "**" + body + 16-bit byte-sum checksum."""
    return b"**" + body + sum16(body)


def crc16(data):
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def with_crc(frame):
    c = crc16(frame)
    return frame + bytes([c & 0xFF, c >> 8])


def aes_encrypt(data, key, iv=None):
    """len(2,BE) [+ seed(4) when iv is None] + AES-CBC(zero-padded data)."""
    out = len(data).to_bytes(2, "big")
    if iv is None:
        seed = os.urandom(4)
        iv = md5(seed)
        out += seed
    padded = data + b"\0" * ((16 - len(data) % 16) % 16)
    enc = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
    return out + enc.update(padded) + enc.finalize()


def aes_decrypt(frame, key, iv=None):
    if len(frame) < 6:
        return None
    n = int.from_bytes(frame[:2], "big")
    if iv is None:
        iv, ct = md5(frame[2:6]), frame[6:]
    else:
        ct = frame[2:]
    if not ct or len(ct) % 16:
        return None
    dec = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
    return (dec.update(ct) + dec.finalize())[:n]


def pub64(public_key):
    n = public_key.public_numbers()
    return n.x.to_bytes(32, "big") + n.y.to_bytes(32, "big")


def pub_from64(b):
    return ec.EllipticCurvePublicNumbers(
        int.from_bytes(b[:32], "big"), int.from_bytes(b[32:], "big"),
        ec.SECP256R1()).public_key()


def sign_raw(private_key, data):
    r, s = decode_dss_signature(private_key.sign(data, ec.ECDSA(hashes.SHA256())))
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def verify_raw(public_key, sig64, data):
    try:
        public_key.verify(
            encode_dss_signature(int.from_bytes(sig64[:32], "big"),
                                 int.from_bytes(sig64[32:], "big")),
            data, ec.ECDSA(hashes.SHA256()))
        return True
    except Exception:
        return False


def check_kex(frame):
    """Return the body of a "**"+body+sum16 frame, or None if the sum is wrong."""
    if not frame or len(frame) < 6 or frame[:2] != b"**":
        return None
    body, got = frame[2:-2], frame[-2:]
    return body if sum16(body) == got else None


def modbus_exception(function, code):
    return with_crc(bytes([0x01, function | 0x80, code]))


# ---- device session ----------------------------------------------------------
class DeviceSession:
    """One client connection. Feed each write to handle(); send what it returns
    as notifications, in order."""

    WAIT_CHALLENGE_REPLY, WAIT_PUBKEY, SECURE, DEAD = range(4)

    def __init__(self, regs, log=print):
        self.regs = regs
        self.log = log
        self.state = self.DEAD
        self.secure_key = None
        self._unsec_key = None
        self._unsec_iv = None
        self._ephemeral = None

    def start(self):
        """Begin the handshake; returns the CHALLENGE notification."""
        challenge = os.urandom(4)
        self._unsec_iv = md5(challenge[::-1])
        self._unsec_key = bytes(a ^ b for a, b in zip(self._unsec_iv, LOCAL_AES_KEY))
        self._ephemeral = ec.generate_private_key(ec.SECP256R1())
        self.secure_key = None
        self.state = self.WAIT_CHALLENGE_REPLY
        self.log("handshake: challenge sent")
        return [kex_frame(bytes([0x01, 0x04]) + challenge)]

    def handle(self, data):
        """One write from the client. Returns the notifications to send back."""
        if self.state == self.WAIT_CHALLENGE_REPLY:
            return self._on_challenge_reply(data)
        if self.state == self.WAIT_PUBKEY:
            return self._on_client_pubkey(data)
        if self.state == self.SECURE:
            return self._on_secure(data)
        return []

    def _fail(self, why):
        self.log(f"handshake failed: {why}")
        self.state = self.DEAD
        self.secure_key = None
        return []

    def _on_challenge_reply(self, data):
        body = check_kex(data)
        # Client echoes iv[8:12] in the clear: ** 02 04 <4 bytes> sum.
        if body is None or body[:2] != b"\x02\x04" or body[2:] != self._unsec_iv[8:12]:
            return self._fail("bad challenge reply")
        self.state = self.WAIT_PUBKEY
        self.log("handshake: challenge accepted, sending device key")
        return [kex_frame(bytes([0x03, 0x01, 0x00])), self._peer_pubkey()]

    def _peer_pubkey(self):
        point = pub64(self._ephemeral.public_key())
        sig = sign_raw(PRIVATE_KEY_L1, point + self._unsec_iv)
        plain = kex_frame(bytes([0x04, 0x80]) + point + sig)
        return aes_encrypt(plain, self._unsec_key, self._unsec_iv)

    def _on_client_pubkey(self, data):
        plain = aes_decrypt(data, self._unsec_key, self._unsec_iv)
        body = check_kex(plain) if plain else None
        if body is None or len(body) != 2 + 128 or body[:2] != b"\x05\x80":
            return self._fail("bad client pubkey frame")
        point, sig = body[2:66], body[66:130]
        if not verify_raw(PUBLIC_KEY_L1, sig, point + self._unsec_iv):
            return self._fail("client signature rejected")
        try:
            shared = self._ephemeral.exchange(ec.ECDH(), pub_from64(point))
        except Exception as exc:
            return self._fail(f"ecdh failed ({exc})")
        if len(shared) != 32:
            return self._fail("ecdh shared secret is not 32 bytes")
        note = aes_encrypt(kex_frame(bytes([0x06, 0x01, 0x00])),
                           self._unsec_key, self._unsec_iv)
        self.secure_key = shared
        self.state = self.SECURE
        self.log("handshake: secure link up")
        return [note]

    def _on_secure(self, data):
        plain = aes_decrypt(data, self.secure_key, None)
        if not plain:
            self.log("secure: undecryptable frame")
            return []
        reply = self._modbus(plain)
        if reply is None:
            return []
        return [aes_encrypt(reply, self.secure_key, None)]

    def _modbus(self, frame):
        if len(frame) < 8 or crc16(frame[:-2]) != int.from_bytes(frame[-2:], "little"):
            self.log(f"modbus: bad crc ({frame.hex()})")
            return None
        func = frame[1]
        addr = int.from_bytes(frame[2:4], "big")
        if func == 0x03:
            qty = int.from_bytes(frame[4:6], "big")
            words = self.regs.read(addr, qty) if 0 < qty <= 125 else None
            if words is None:
                self.log(f"FC3 {addr} x{qty} -> exception")
                return modbus_exception(0x03, 0x02)
            self.log(f"FC3 {addr} x{qty}")
            body = bytes([0x01, 0x03, len(words) * 2])
            for w in words:
                body += int(w).to_bytes(2, "big")
            return with_crc(body)
        if func == 0x06:
            value = int.from_bytes(frame[4:6], "big")
            if not self.regs.write(addr, value):
                self.log(f"FC6 {addr} = {value} -> exception")
                return modbus_exception(0x06, 0x02)
            self.log(f"FC6 {addr} = {value}")
            return frame[:8]
        self.log(f"modbus: unsupported function {func}")
        return modbus_exception(func, 0x01)
