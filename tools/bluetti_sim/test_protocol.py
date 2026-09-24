"""DeviceSession against a Python port of the HMI's client (src/bluetti_crypt.cpp).

Run from the repo root:  tools\\bluetti_sim\\.venv\\Scripts\\python -m pytest tools\\bluetti_sim
"""
import time

import pytest
from cryptography.hazmat.primitives.asymmetric import ec

import registers as R
from protocol import (LOCAL_AES_KEY, PRIVATE_KEY_L1, PUBLIC_KEY_L1, DeviceSession,
                      aes_decrypt, aes_encrypt, crc16, kex_frame, md5, pub64,
                      pub_from64, sign_raw, verify_raw, with_crc)

PUBLIC_KEY_K2 = pub_from64(bytes.fromhex(
    "A73ABF5D2232C8C1C72E68304343C272495E3A8FD6F30EA96DE2F4B3CE60B251"
    "EE21AC667CF8A71E18B46B664EAEFFE3C489F24F695B6411DB7E22CCC85A8594"))


class HmiClient:
    """Line-for-line port of BluettiCrypt::handle() and its helpers.
    handle() returns (code, reply, plain) with the firmware's codes:
    -1 error, 0 ignore, 1 send reply, 2 ready, 3 Modbus response in plain."""

    def __init__(self, verify_key):
        self.verify_key = verify_key
        self.unsec_key = self.unsec_iv = self.sec_key = None
        self.ready = False

    def handle(self, data):
        if len(data) < 4:
            return -1, None, None
        if data[:2] == b"**":
            if data[2] == 1:
                if len(data) < 10:
                    return -1, None, None
                self.unsec_iv = md5(data[4:8][::-1])
                self.unsec_key = bytes(a ^ b for a, b in zip(self.unsec_iv, LOCAL_AES_KEY))
                return 1, kex_frame(bytes([0x02, 0x04]) + self.unsec_iv[8:12]), None
            if data[2] == 3:
                return 0, None, None
        if self.unsec_key is None:
            return -1, None, None
        if self.ready:
            dec = aes_decrypt(data, self.sec_key, None)
        else:
            dec = aes_decrypt(data, self.unsec_key, self.unsec_iv)
        if not dec:
            return -1, None, None
        if len(dec) >= 6 and dec[:2] == b"**":
            payload, plen = dec[4:], len(dec) - 6
            if dec[2] == 4:
                if plen < 128:
                    return -1, None, None
                return self._on_peer_pubkey(payload[:128])
            if dec[2] == 6:
                if plen < 1 or payload[0] != 0:
                    return -1, None, None
                self.sec_key = self.my.exchange(ec.ECDH(), self.peer)
                self.ready = True
                return 2, None, None
            return 0, None, None
        return 3, None, dec

    def _on_peer_pubkey(self, p):
        if not verify_raw(self.verify_key, p[64:128], p[:64] + self.unsec_iv):
            return -1, None, None
        self.peer = pub_from64(p[:64])
        self.my = ec.generate_private_key(ec.SECP256R1())
        mine = pub64(self.my.public_key())
        sig = sign_raw(PRIVATE_KEY_L1, mine + self.unsec_iv)
        msg = kex_frame(bytes([0x05, 0x80]) + mine + sig)
        return 1, aes_encrypt(msg, self.unsec_key, self.unsec_iv), None

    def read_cmd(self, addr, qty):
        f = bytes([1, 3]) + addr.to_bytes(2, "big") + qty.to_bytes(2, "big")
        return aes_encrypt(with_crc(f), self.sec_key, None)

    def write_cmd(self, addr, val):
        f = bytes([1, 6]) + addr.to_bytes(2, "big") + val.to_bytes(2, "big")
        return aes_encrypt(with_crc(f), self.sec_key, None)


def pair(dev, cli):
    """Pump notifications/writes the way connectAndHandshake() does."""
    inbox = list(dev.start())
    while inbox and not cli.ready:
        code, reply, _ = cli.handle(inbox.pop(0))
        if code < 0:
            return False
        if code == 1:
            inbox += dev.handle(reply)
    return cli.ready


def modbus(dev, cli, cmd):
    out = dev.handle(cmd)
    assert len(out) == 1
    code, _, plain = cli.handle(out[0])
    assert code == 3
    assert crc16(plain[:-2]) == int.from_bytes(plain[-2:], "little")
    return plain


def parse_read(plain, qty):
    """modbus_parse_read(): words, or None for anything but a matching FC3 reply."""
    if plain[:2] != b"\x01\x03" or plain[2] != qty * 2:
        return None
    return [int.from_bytes(plain[3 + 2 * i:5 + 2 * i], "big") for i in range(qty)]


@pytest.fixture
def linked():
    regs = R.Registers()
    regs.commit_delay = 0
    dev, cli = DeviceSession(regs, log=lambda *_: None), HmiClient(PUBLIC_KEY_L1)
    assert pair(dev, cli)
    return regs, dev, cli


def test_pairs_with_sim_build(linked):
    _, dev, cli = linked
    assert dev.state == DeviceSession.SECURE
    assert dev.secure_key == cli.sec_key


def test_normal_build_rejects_it():
    dev, cli = DeviceSession(R.Registers(), log=lambda *_: None), HmiClient(PUBLIC_KEY_K2)
    assert not pair(dev, cli)


def test_notifications_fit_the_hmi_frame_buffer():
    # The HMI's notify queue holds frames up to 256 bytes (Frame::data).
    dev, cli = DeviceSession(R.Registers(), log=lambda *_: None), HmiClient(PUBLIC_KEY_L1)
    sizes, inbox = [], list(dev.start())
    while inbox and not cli.ready:
        n = inbox.pop(0)
        sizes.append(len(n))
        code, reply, _ = cli.handle(n)
        if code == 1:
            inbox += dev.handle(reply)
    assert max(sizes) <= 256


def test_bad_challenge_reply_kills_the_session():
    dev = DeviceSession(R.Registers(), log=lambda *_: None)
    dev.start()
    assert dev.handle(kex_frame(bytes([0x02, 0x04]) + b"\0\0\0\0")) == []
    assert dev.state == DeviceSession.DEAD


def test_poll_block_read(linked):
    regs, dev, cli = linked
    regs.set(R.DC_OUT_W, 18)
    regs.set(R.AC_OUT_W, 44)
    regs.set(R.DC_IN_W, 120)
    regs.set(R.AC_IN_W, 1261)
    w = parse_read(modbus(dev, cli, cli.read_cmd(140, 8)), 8)
    assert (w[0], w[2], w[4], w[6]) == (18, 44, 120, 1261)


def test_unreadable_register_is_an_exception(linked):
    _, dev, cli = linked
    plain = modbus(dev, cli, cli.read_cmd(5000, 1))
    assert plain[1] == 0x83 and parse_read(plain, 1) is None


def test_write_echoes_then_commits_after_delay(linked):
    regs, dev, cli = linked
    regs.commit_delay = 0.3
    cmd_plain = with_crc(bytes([1, 6]) + (2011).to_bytes(2, "big") + (0).to_bytes(2, "big"))
    assert modbus(dev, cli, cli.write_cmd(2011, 0)) == cmd_plain
    assert parse_read(modbus(dev, cli, cli.read_cmd(2011, 1)), 1) == [1]
    time.sleep(0.35)
    assert parse_read(modbus(dev, cli, cli.read_cmd(2011, 1)), 1) == [0]


def test_output_off_zeroes_its_watts(linked):
    regs, dev, cli = linked
    regs.set(R.AC_OUT_W, 500)
    regs.set(R.CTRL_AC, 0)
    assert parse_read(modbus(dev, cli, cli.read_cmd(142, 1)), 1) == [0]


def test_energy_counters_decode_like_the_firmware(linked):
    regs, dev, cli = linked
    regs.set_energy(R.AC_OUT_ENERGY, 41.3)
    regs.set_energy(R.PV_CHG_ENERGY, 12.8)
    regs.set_energy(R.GRID_CHG_ENERGY, 7000.4)  # > 6553.5 kWh, so the high word is used
    w = parse_read(modbus(dev, cli, cli.read_cmd(152, 6)), 6)
    # bluetti.cpp: value = (w[n+1] << 16) | w[n], in 0.1 kWh
    assert [(w[i + 1] << 16) | w[i] for i in (0, 2, 4)] == [413, 128, 70004]


def test_energy_counters_build_from_power_flow():
    regs = R.Registers()
    regs.set_energy(R.GRID_CHG_ENERGY, 0)
    regs.set(R.AC_IN_W, 1200)
    regs.tick(3600)  # one hour at 1200 W
    assert abs(regs.get_energy(R.GRID_CHG_ENERGY) - 1.2) < 1e-9
