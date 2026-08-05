# Bluetti Elite 300 — native BLE integration

The HMI (ESP32‑S3) talks to the Bluetti Elite 300 **directly over BLE** and
decrypts the telemetry **on‑device**. No bridge, no Raspberry Pi, no cloud, and
**no licence file** — the encryption is the open‑source community implementation,
which uses hardcoded universal keys.

> History: we first tried the official `bluetti-official/bluetti-bluetooth-lib`,
> but its crypto is a closed **x86‑64‑only** binary (`_bluetti_crypt.so/.pyd`) that
> can't run on ARM/ESP32. The open reimplementation in
> [`Patrick762/bluetti-bt-lib`](https://github.com/Patrick762/bluetti-bt-lib)
> (originally RE'd by `nhurman/bluetti_mqtt`) made the native port possible.

## Device / BLE

- Advertised name: **`EL3002609112693584`** (prefix `EL300`); MAC `1c:db:d4:92:5b:aa`.
- Vendor GATT **service `0xFF00`**:
  - `0xFF01` — NOTIFY (device → app)
  - `0xFF02` — WRITE  (app → device)
- **Single BLE client**: while one client (HMI or phone app) is connected the
  device won't accept another — hence the "release for app" button (below).

## Encryption ("v2" protocol)

Keys are **universal** (from the RE'd app, not per‑device — see
`src/bluetti_crypt.cpp`):

| Const | Hex | Use |
|---|---|---|
| `LOCAL_AES_KEY` | `459FC535808941F17091E0993EE3E93D` | derive unsecure key |
| `PRIVATE_KEY_L1` | `4F19A16E…F56AF337` (32 B) | ECDSA‑P256 sign our pubkey |
| `PUBLIC_KEY_K2` | point `A73ABF5D…A8594` (64 B, X‖Y) | verify the device's pubkey |

Frames in the key‑exchange phase are `2A2A` ("**") + body + 2‑byte checksum
(low 16 bits of the byte sum). Handshake, driven by device notifications:

1. **CHALLENGE** (type 1, 4 B): `unsecure_iv = MD5(reverse(4B))`,
   `unsecure_key = unsecure_iv XOR LOCAL_AES_KEY`. Reply `** 0204 iv[8:12] sum16`.
2. **CHALLENGE_ACCEPTED** (type 3): ignore.
3. **PEER_PUBKEY** (type 4, AES‑encrypted with unsecure key/iv; 64‑B pubkey +
   64‑B sig): verify ECDSA‑P256‑SHA256 over `pubkey‖unsecure_iv` with `K2`; load
   the peer point (`04‖64B`); generate an ephemeral P‑256 keypair; sign
   `mypub64‖unsecure_iv` with `L1`; reply `** 0580 mypub64 rawsig64` (encrypted).
4. **PUBKEY_ACCEPTED** (type 6, data `00`): `secure_key = ECDH(myPriv, peerPub)`
   (32‑byte X coordinate). Link is now ready.

AES‑CBC framing:
- **Unsecure** phase: AES‑128 with the fixed `unsecure_iv`; frame = `len(2,BE) +
  ciphertext`.
- **Secure** phase: AES‑256 with `secure_key`; **per‑message IV** = `MD5(seed)`;
  frame = `len(2,BE) + seed(4) + ciphertext`.

All implemented with the ESP32's bundled **mbedTLS** (`mbedtls_aes`, `md5`,
`sha256`, `ecp`/`ecdsa`/`ecdh` on `SECP256R1`). The Python `scratchpad` reference
(`test_open.py`) was the golden model for the C port.

## Modbus

Commands ride inside the encrypted channel as standard Modbus RTU. Pure framing
lives in **`include/logic/modbus.h`** (unit‑tested):
- **FC3 read**: `01 03 addrHi addrLo qtyHi qtyLo crcLo crcHi`
- **FC6 write**: `01 06 addrHi addrLo valHi valLo crcLo crcHi`
- CRC16 (poly `0xA001`, init `0xFFFF`), little‑endian on the wire.
- FC3 response: `01 03 byteCount <words…> crc`; exception: `01 83 code crc`.

## Register map (Elite 300 / v2)

Read (holding registers, big‑endian words):

| Reg | Field | Scale / notes |
|---|---|---|
| 102 | State of charge | % |
| 104 | Time remaining | raw; PR uses ×1/60 — **scaling unverified** |
| 110–115 | Device type | swap‑string ("EL300") |
| 116–119 | Serial number | |
| **140** | **DC output power** | W |
| **142** | **AC output power** | W (verified: tracks AC load) |
| **144** | **DC input power** | W |
| **146** | **AC input power** | W |
| 152 | Battery temperature | °C (confirmed under load; read into `tempC`) |
| 154 | Lifetime generation | ×0.1 kWh |
| 1314 / 1315 | AC input voltage / current | ×0.1 |
| 1432 / 1470 | AC output current / frequency | ×0.1 |

Control (write FC6, **1 = on, 0 = off**) — both **confirmed live**:

| Reg | Field |
|---|---|
| **2011** | `CTRL_AC` — AC output on/off (1/0) |
| **2012** | `CTRL_DC` — DC output on/off (1/0) |
| **2020** | charging mode: **0=Standard, 1=Silent, 2=Turbo, 4=Custom** (3 unused) |
| **2214** | Custom mode max grid charging current (A) — e.g. 3 = 3 A |
| **2014** | DC ECO enable (1/0) |
| **2017** | AC ECO enable (1/0) |
| **2021** | Power Lifting enable (1/0) |
| **2067** | Screen Timeout enum: **2=30 s, 3=1 min, 4=5 min, 5=Never** (1 min inferred) |

(Confirmed by app-toggle diffs, `scratchpad/btest/diff_regs.py` (config zone) and
`scratchpad/btest/wide.py` (full 0–6000 block sweep — needed for 2067, which sits
outside the originally-dumped windows). The ECO 4 h / W-threshold sub-settings live
elsewhere — not captured. **Visitor Access** is an app/cloud permission, not a
device register.)

Discovery method: read‑correlate a register against the unit's real state while
toggling it (see `scratchpad/btest/discover_ctrl.py` / `dump_regs.py`). The
official `.so` enforced a register **whitelist**; the open crypto has no such
limit, so any valid register is readable.

### Full dump (2026-06-25, SoC 89 %, idle)

A sweep of readable registers (`dump_regs.py`, ranges 100–170 / 1300–1500 /
2000–2050 / 2200–2260 / 3000–3100). Beyond the confirmed fields above, these are
**candidates pending live correlation** (raw values at idle):

Confirmed under a 44 W AC load (2026-06-25):

| Reg | Meaning |
|---|---|
| **142** | AC output power W — read 44, matched the unit exactly |
| **104** (=105) | **TIME_REMAINING in MINUTES** — 1987 ≈ 33 h at 44 W (firmware treats reg 104 as minutes; hidden when idle/huge) |
| **152** | **battery temperature °C** — 24→25 as it warmed under load |
| **1431** | AC output voltage ×0.1 (~230.8 V) |
| **1432** | AC output current ×0.1 (0.4 A under load) |
| **169** | AC output voltage (integer V) — mirrors 1431 |

Confirmed under mains AC charging (1261 W):

| Reg | Meaning |
|---|---|
| **146** | AC input power W — read 1261 (mains charge rate) |
| **1314** | AC input voltage ×0.1 (243 V mains) |
| **1315** | AC input current ×0.1 (5.2 A; 1261 W ÷ 243 V ✓) |
| **104** | now counts down to FULL (19 min at 88 %); charging flag = inputs>outputs |

(DC-in regs still unmapped — need the Charger 2 / solar feeding the 12–60 V DC input,
not AC mains, to identify DC input voltage/current.)

Still unconfirmed:

| Reg | Raw | Likely meaning |
|---|---|---|
| 2022 / 2023 | 20 / 80 | battery SoC limits (discharge floor / charge ceiling %) |
| 2213 | 2400 | rated AC power (W) |
| 167 | ~6540 | slowly rising counter (energy/runtime?) |
| 2258 / 2259 | 80 / 300 | unknown |
| 2001 / 2002 / 2003 | 6662 / 6416 / 5914 | unknown (pack/cell?) |
| 148 / 149, 2211 / 2212 | 65535 | unused / signed −1 |
| 3000–3036 | 0 | NOT live power — stayed 0 under load; unknown/charge-only |

Misc small/config (unidentified): 100=996, 101=19, 107=108=1, 121=122=1, 124=6,
150=7, 156=6, 161=1, 167=6540, 2013=3, 2015=4, 2016=5, 2018=4, 2019=10,
2030/2033/2036/2039/2042/2045=3, 2207=2209=1, 2214=1, 2218=3, 2242=2.
Full raw dump: `scratchpad/btest/regdump.txt`.

### Wide sweep (2026-06-25, full 0–6000 block scan)

`scratchpad/btest/wide.py` block-reads 0–6000 (qty 10) and found **940 readable
registers** — far more than the curated dump. This is how reg 2067 (Screen Timeout)
was located; it lives outside every range the original dump covered. Readable
windows: `0–19, 100–199, 700–759, 1100–1179, 1200–1339, 1400–1469, 1500–1559,
1600–1609, 2000–2089, 2200–2279, 2400–2449, 2500–2539, 3000–3029, 3500–3549,
3600–3659`. Raw baseline saved at `scratchpad/btest/wide_base.json`.

**Newly confirmed / decoded:**

| Reg(s) | Field | Notes |
|---|---|---|
| 110–115 | Device type | swap-string = `EL300` |
| **116–119** | **Serial number** | LE 16-bit words → `2609112693584`; matches advert `EL300`+serial. Mirrored at 1107–1109 |
| **1500** | **AC output frequency** | ×0.1 Hz — read 499 = 49.9 Hz (idle; strong candidate) |
| 1155 / 2213 | Rated AC power | both = 2400 (W) |
| **2083** | **Charge limit %** | high byte = percent (write `pct << 8`): 100 %=`0x6400`, 85 %=`0x5500`. The app's only charge-limit slider — confirmed by 100→85 diff |

**Strong candidates (idle values, not yet load-correlated):**

| Reg | Raw | Likely meaning |
|---|---|---|
| 2022 / 2023 | 20 / 80 | unidentified — **not** the user charge limit (that's 2083); possibly internal protection defaults |
| 1511 | ~2301 | a second AC voltage reading ×0.1 (drifts live, like 1431) |
| 2258 / 2259 / 2261–2265 | 80 / 300 / 330 / 100 … | grid/charge config pairs (330/100 repeat) — unidentified |

**Readable but all-zero (don't re-investigate):** `700–759, 2400–2449, 2500–2539,
3000–3029, 3500–3549, 3600–3659` returned 0 across the board (3000-block already
known to be non-live). `148/149, 2211/2212` read 65535 (unused / signed −1).

**Cross-checked against the reference lib's `AC180` (V2, same family) — confirms:**
1500 = AC output frequency ×0.1, 1511 = AC output voltage ×0.1 (we read 1431 for
the same; both track ~230 V). The lib also maps, for that family: 1300 = AC input
frequency, 1213 / 1214 = **DC input voltage / current** ×0.1, 1314 / 1315 = AC
input V / I. The 1213/1214 pair is the most likely home for the still-unmapped
**DC-in (Charger 2 / solar)** reading — confirm when the DC input is feeding.
Notably the lib does **not** know the Elite's screen-timeout register (commented
`# Display timeout (?)`) — we found it (2067).

## Cooling fan / grid-connected — best guess, unconfirmed (2026-08-05)

Wanted to mirror two icons the Bluetti's own screen shows (fan running, grid/
mains detected). Method: swept the known-readable config/status blocks
(`100–199`, `2000–2089`, `2200–2279`, block reads of 10) once idle, again
while AC-charging with the fan audibly running, and diffed. Two clean
small-integer flips stood out from the noise (SoC, time-remaining, AC input
power all moved too, as expected):

| Reg | Idle | Charging + fan on | Assigned to |
|---|---|---|---|
| **103** | 0 | 1 | `power.fanOn` — clean 0/1 boolean, picked as the fan (simple sensor-driven flags tend to be plain on/off) |
| **161** | 0 | 2 | `power.gridConnected` — jumps by 2 rather than 1, read as a multi-state input-detect enum (0=absent, wasn't able to catch a "1" state, 2=detected+drawing) rather than a simple toggle, picked as grid/mains |

**This has NOT been independently verified** — both registers changed
together in the same window (charging started and the fan came on close
together), so the diff alone can't prove which is which. If the two icons
are backwards on the actual hardware, swap the register numbers in
`power.h`/`bluetti.cpp` (search `fanOn`/`gridConnected`) and here.

Other registers that changed in the same diff, for reference (not wired to
anything): `100` 1000→1013 (drifts even at idle, looks like a counter), `101`
2→732 (large jump, maybe a duration/energy accumulator), `156` 24→25
(temperature-like, +1 under load), `188` 0→209 (unclear), `2003` 4916→5915
(the already-unidentified pack/cell register, +999), `148` 65535→64745 (the
already-known unused/−1 sentinel register, now reading roughly −791 signed —
plausibly a net power-flow value, unconfirmed).

## Sleep / standby (2026-06-25)

The app's power button offers **Sleep** or **Full power off**. In **Sleep** the unit
still answers BLE (slower to connect, ~10 s vs ~2 s).

**There is no reliable sleep flag, and no usable wake command — the feature was
removed.** Details, so we don't re-try the same dead ends:

- **`2073` is NOT a clean sleep indicator.** It reads 4 when freshly running and
  flips to 5 in **Sleep** — but it *also* reads 5 whenever **both outputs are off**
  in normal operation, and it's **sticky**: once 5, it stays 5 even after you turn
  an output back on (writes succeed regardless). So 2073==5 can't tell "asleep"
  apart from "awake, outputs off". An early build gated the Power UI on 2073==5 and
  it **locked out AC/DC control** whenever both outputs were off — reverted.
  (`2013` behaves similarly / unreliably.)
- **Wake over BLE not found.** A curated FC6 sweep of `2011/2012`, `2013`, `2073`,
  `3007/3008/3060`, `2060`, `2076` (various values) had **no effect** while asleep
  (`wakehunt.py`). The phone app *does* wake the unit locally over Bluetooth, so a
  command exists — likely a **write-only command register** (the all-zero ranges
  2400–2449 / 2500–2539) and/or a **different function code** (FC16). Not pursued.

Net: the HMI does not detect or control sleep. When the unit is asleep + the link
drops it simply shows the normal "offline/connecting" state.

## Firmware

| File | Role |
|---|---|
| `src/bluetti_crypt.{h,cpp}` | mbedTLS handshake + AES + Modbus command build (uses `logic/modbus.h`) |
| `src/bluetti.{h,cpp}` | NimBLE client + poller task; fills `power`; AC/DC writes; app‑release |
| `src/power.{h,cpp}` | shared `PowerData` + `power_valid()` / `power_soc()` |
| `include/logic/modbus.h` | pure, unit‑tested Modbus/CRC/checksum helpers |
| `src/ui.cpp` | Power screen (gauge, cards, toggles, release button) |
| `src/settings.{h,cpp}` | `bluettiMac` (NVS key `btmac`; empty = auto‑discover) |

Runtime design (in `bluetti.cpp`):
- A FreeRTOS task (core 0, 20 KB stack) keeps **one persistent connection**:
  connect + handshake once, reuse for all ops, reconnect only if dropped
  (`ensureConnected`). This was the key reliability fix — per‑op reconnect was
  flaky.
- Polls every **3 s** (`POLL_MS`) — this fork has no WiFi to share the 2.4 GHz
  radio with, so it polls faster than the parent multi-feature project's 15 s;
  retries after **4 s** on failure. Each read/write retries up to **3×**
  (occasional lost BLE notifications).
- Polls: `102→soc`, `104→ttfMin`, `140‑block (qty 8)→dcOut/acOut/dcIn/acIn`,
  `2011→acOn`, `2012→dcOn`. `charging = inputs > outputs`. `whRemaining` not mapped.
- `power_valid()` tolerates **60 s** of staleness (rides out brief BLE gaps).
- **Release for app**: `bluetti_release(seconds)` drops the link and pauses polling
  so the phone app can connect; auto‑resumes after the window.
- Coexists with WiFi (weather) and ESP‑NOW (switches).
- Verbose logging is behind `#define BLUETTI_DEBUG 0` in `bluetti.cpp`.

### Control writes — reliability (hard‑won, 2026‑06‑26)
- **Commit lag:** the Elite 300 *echoes* an FC6 write immediately but **commits the
  value ~0.4 s later**, so an instant read‑back still shows the old value. `writeReg()`
  therefore writes, then **re‑reads the register until it reflects the target** (a few
  300 ms polls); that read‑back — not the echo — is the real success signal.
- **UI:** optimistic updates were removed. A control reflects a change only once a poll
  confirms it; meanwhile `ui.cpp` shows a centred **spinner** (`drawPendingSpinner`,
  cleared when `power.fetchedMs` advances or after a 6 s timeout).
- **⚠️ The big gotcha:** `writeReg()` was once *called inside* a `BDBG(...)` argument,
  which expands to `do{}while(0)` when `BLUETTI_DEBUG=0` — so in **release builds the
  call was never evaluated and no command was sent** ("only works with debug on"). It
  cost a long debug session. Evaluate side‑effecting calls *before* logging them.
- **BLE command spacing:** a small gap before each command (`BLE_GAP_MS`, ~40 ms) — the
  device is unreliable with back‑to‑back commands; it also cut connect retries.
- **WiFi modem‑sleep disabled** (`WiFi.setSleep(false)` in `comms_init`): WiFi + BLE
  share the 2.4 GHz radio and modem‑sleep disrupts the timing‑sensitive link.
- **Fast reconnect after reboot:** the Elite 300 is a single‑client peripheral and
  holds the stale link for its full supervision timeout after an ungraceful reboot
  (~20 s+ of "can't reconnect"). We request a **short ~4 s supervision timeout** via
  `g_client->setConnectionParams(24, 40, 0, 400)` (30–50 ms interval, 0 latency, 4 s
  timeout) so the dead link clears quickly and we reconnect on the next retry.

## Power screen (UI)

Full-height, no status bar — this is the app's home screen.

- Centre ring gauge: SoC % + remaining time.
- Four corner cards — `DC IN`/`AC IN` (display) and `DC OUT`/`AC OUT` which
  **double as on/off toggles** (green border + a bottom-left dot when on,
  grey border + no dot when off) with a confirm step.
- Settings gear in the gap between `AC IN` and `AC OUT` -> Bluetti Settings.
- Bluetooth link icon (flashes while connecting) only shown on the "Bluetti
  offline" state — the only time link status isn't otherwise visible.
- Bluetti Settings carries a top utility row — history chart + **release-for-
  app** (phone icon, 90 s window with countdown) — always available
  regardless of connection state, above the ECO/charge-limit/timeout/pairing
  rows that need live data.

## Tests

`include/logic/modbus.h` is covered by `test/test_logic/test_main.cpp`
(CRC vector, frame build, response parse + validation, write‑echo, helpers).

```
pio test -d HMI_Screen -e native      # needs host gcc (MinGW on Windows)
```

Also gated in CI: `.github/workflows/test.yml`. The crypto (AES/ECDH) is not
host‑unit‑tested (needs mbedTLS) — it's validated live against the device and the
Python reference.

## References

- Open lib + register defs: <https://github.com/Patrick762/bluetti-bt-lib>
  (`bluetti_bt_lib/bluetooth/encryption.py`, `devices/el30v2.py`/`el100v2.py`;
  PR #49 = E200V2 fields, #59 = encrypted writes, #60 = persistent connection).
- Original protocol RE: `nhurman/bluetti_mqtt`.
- Official (closed, unused): <https://github.com/bluetti-official/bluetti-bluetooth-lib>.
