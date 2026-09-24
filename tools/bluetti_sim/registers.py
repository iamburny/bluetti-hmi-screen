"""Register model for the simulated Elite 300.

Holds the Modbus holding registers the HMI reads and writes. Register numbers
and scaling follow docs/BLUETTI.md. The GUI thread and the BLE thread both use
it, so every access goes through one lock.
"""
import threading
import time

# Readable windows from the wide sweep in docs/BLUETTI.md (end exclusive).
# Anything outside them answers with Modbus exception 02, like the real unit.
READABLE = [
    (0, 20), (100, 200), (700, 760), (1100, 1180), (1200, 1340),
    (1400, 1470), (1500, 1560), (1600, 1610), (2000, 2090), (2200, 2280),
    (2400, 2450), (2500, 2540), (3000, 3030), (3500, 3550), (3600, 3660),
]

CAPACITY_WH = 3024

# Register numbers.
SOC = 102
TIME_REMAINING = 104      # minutes
DC_OUT_W, AC_OUT_W, DC_IN_W, AC_IN_W = 140, 142, 144, 146
BATTERY_TEMP = 156        # degC
AC_OUT_DV = 1431          # volts x10
AC_OUT_FREQ_DHZ = 1500    # Hz x10
CTRL_AC, CTRL_DC = 2011, 2012
DC_ECO, AC_ECO = 2014, 2017
CHARGE_MODE = 2020        # 0 Standard, 1 Silent, 2 Turbo, 4 Custom
POWER_LIFT = 2021
SCREEN_TIMEOUT = 2067     # 2 = 30 s, 3 = 1 min, 4 = 5 min, 5 = Never
CHARGE_LIMIT = 2083       # percent in the high byte
GRID_CHARGE_A = 2214

# Idle values seen on a real unit (docs/BLUETTI.md "Full dump"), so a
# Diagnostics sweep on the HMI sees a plausible register map, not all zeros.
_IDLE = {
    100: 996, 101: 19, 107: 1, 108: 1, 121: 1, 122: 1, 150: 7, 167: 6540,
    1155: 2400, 2001: 6662, 2002: 6416, 2003: 5914, 2013: 3, 2015: 4, 2016: 5,
    2018: 4, 2019: 10, 2022: 20, 2023: 80, 2207: 1, 2209: 1, 2213: 2400,
    2218: 3, 2242: 2, 2258: 80, 2259: 300,
}

# Registers derived from the others on every read, never stored directly.
_DERIVED = {103, 105, 124, 140, 142, 148, 149, 161, 169, 1314, 1315, 1400,
            1420, 1432, 1511}


def _swap_string(s, words):
    b = s.encode().ljust(words * 2, b"\0")
    return [(b[i + 1] << 8) | b[i] for i in range(0, words * 2, 2)]


def readable(addr):
    return any(lo <= addr < hi for lo, hi in READABLE)


class Registers:
    def __init__(self):
        self._lock = threading.Lock()
        self._r = dict(_IDLE)
        for i, w in enumerate(_swap_string("EL300", 6)):
            self._r[110 + i] = w
        serial = 2300000000001  # dummy 13-digit serial, little-endian words
        for i in range(4):
            self._r[116 + i] = (serial >> (16 * i)) & 0xFFFF
        self._r.update({
            SOC: 80, TIME_REMAINING: 0, DC_OUT_W: 0, AC_OUT_W: 0, DC_IN_W: 0,
            AC_IN_W: 0, BATTERY_TEMP: 24, AC_OUT_DV: 2300, AC_OUT_FREQ_DHZ: 500,
            CTRL_AC: 1, CTRL_DC: 1, DC_ECO: 0, AC_ECO: 0, CHARGE_MODE: 0,
            POWER_LIFT: 0, SCREEN_TIMEOUT: 3, CHARGE_LIMIT: 100 << 8,
            GRID_CHARGE_A: 3,
        })
        self._pending = []            # (due, addr, value) writes not yet committed
        self._soc_f = float(self._r[SOC])
        self.commit_delay = 0.4       # seconds from a write's echo to its commit
        self.simulate_battery = False
        self.on_commit = None         # callback(addr, value) after an HMI write lands

    # ---- GUI side ---------------------------------------------------------
    def get(self, addr):
        with self._lock:
            return self._r.get(addr, 0)

    def set(self, addr, value):
        with self._lock:
            self._r[addr] = int(value) & 0xFFFF
            if addr == SOC:
                self._soc_f = float(self._r[SOC])

    # ---- BLE side ---------------------------------------------------------
    def read(self, addr, qty):
        """Words for an FC3 read, or None if any register isn't readable."""
        if not all(readable(a) for a in range(addr, addr + qty)):
            return None
        self.commit_due()
        with self._lock:
            return [self._value(a) for a in range(addr, addr + qty)]

    def write(self, addr, value):
        """Queue an FC6 write; it takes effect after commit_delay."""
        if not readable(addr):
            return False
        with self._lock:
            self._pending.append((time.monotonic() + self.commit_delay, addr, value))
        return True

    def commit_due(self):
        now = time.monotonic()
        # Apply in the same critical section that dequeues, so two callers
        # can't land queued writes to one register out of order.
        with self._lock:
            due = [p for p in self._pending if p[0] <= now]
            self._pending = [p for p in self._pending if p[0] > now]
            for _, addr, value in due:
                self._r[addr] = value & 0xFFFF
                if addr == SOC:
                    self._soc_f = float(value)
        for _, addr, value in due:
            if self.on_commit:
                self.on_commit(addr, value)

    def tick(self, dt):
        """Advance the battery model by dt seconds (when enabled)."""
        self.commit_due()
        if not self.simulate_battery:
            return
        with self._lock:
            net = self._net_w()
            limit = self._r[CHARGE_LIMIT] >> 8
            self._soc_f += net * dt / 3600 / CAPACITY_WH * 100
            self._soc_f = max(0.0, min(float(limit if net > 0 else 100), self._soc_f))
            self._r[SOC] = int(round(self._soc_f))
            if net > 0:
                mins = (limit - self._soc_f) / 100 * CAPACITY_WH / net * 60
            elif net < 0:
                mins = self._soc_f / 100 * CAPACITY_WH / -net * 60
            else:
                mins = 0
            self._r[TIME_REMAINING] = max(0, min(0xFFFF, int(mins)))

    # ---- internals (caller holds the lock) --------------------------------
    def _ac_out(self):
        return self._r[AC_OUT_W] if self._r[CTRL_AC] else 0

    def _dc_out(self):
        return self._r[DC_OUT_W] if self._r[CTRL_DC] else 0

    def _net_w(self):
        return (self._r[DC_IN_W] + self._r[AC_IN_W]) - (self._dc_out() + self._ac_out())

    def _value(self, a):
        if a not in _DERIVED:
            return self._r.get(a, 0)
        ac_in, ac_out, dc_out = self._r[AC_IN_W], self._ac_out(), self._dc_out()
        if a in (140, 1400):
            return dc_out
        if a in (142, 1420):
            return ac_out
        if a == 161:  # bit1 = AC input present, bit0 = AC output active
            return (2 if ac_in else 0) | (1 if ac_out else 0)
        if a == 103:  # 1 = net charging, 2 = net discharging
            net = self._net_w()
            return 1 if net > 0 else 2 if net < 0 else 0
        if a == 105:
            return self._r[TIME_REMAINING]
        if a == 124:
            return 2 if ac_out else 0
        if a in (148, 149):  # signed AC power, 32-bit: + output, - input
            v = (ac_out - ac_in) & 0xFFFFFFFF
            return v & 0xFFFF if a == 148 else v >> 16
        if a == 169:
            return self._r[AC_OUT_DV] // 10
        if a == 1511:
            return self._r[AC_OUT_DV]
        if a == 1432:  # AC output amps x10
            dv = self._r[AC_OUT_DV] or 1
            return int(ac_out * 100 / dv)
        if a == 1314:
            return 2430 if ac_in else 0
        if a == 1315:
            return int(ac_in * 100 / 2430) if ac_in else 0
        return 0
