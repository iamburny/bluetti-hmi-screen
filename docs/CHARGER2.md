# Bluetti Charger 2 — HMI charge on/off control (PLAN, not yet implemented)

**Goal:** enable/disable charging on the **Bluetti Charger 2** (1200 W Alternator &
Solar dual DC charger, launched CES Jan 2026) from the HMI touch panel — the same
way the HMI already controls the Elite 300's AC/DC outputs.

**Status (2026-06-30):** RESEARCH ONLY. Nothing built. This is the field-work plan
to run **out at the van where the Charger 2 is fitted**, with the device powered and
the engine running (it only accepts control while the alternator/D+ signal is live).

> This doc is written to be self-contained so a **fresh Claude Code instance on the
> laptop** can pick it up cold. Read alongside `HMI_Screen/docs/BLUETTI.md` (the full
> protocol/crypto/register reference) — the Charger 2 almost certainly reuses that
> exact stack.

---

## Why there's no shortcut (what's already been checked)

The Charger 2 is **not reverse-engineered anywhere**. Confirmed 2026-06-30 by listing
every device definition in all repos this project relies on:

- `Patrick762/bluetti-bt-lib` (our crypto + register source) — AC/EP/EL/PR power
  stations only; Elite 300 = `el30v2.py`. **No charger device.**
- `nhurman/bluetti_mqtt` (original RE) — repo 404 / gone.
- `warhammerkid/bluetti_mqtt` (active fork) — AC/EP power stations only. **No charger.**
- `bluetti-official/bluetti-bluetooth-lib` (closed) — same power-station list. **No charger.**

So the register that toggles charging on the Charger 2 has to be **discovered on the
physical device**. The good news: the hard 90% (BLE transport + v2 encrypted handshake
+ Modbus framing) is already working on-device for the Elite 300 and is very likely
reusable as-is.

## Working assumption

The Charger 2 is a same-era Bluetti BLE device using the **same `0xFF00` GATT service
(`0xFF01` NOTIFY / `0xFF02` WRITE), the same v2 encrypted handshake with universal
keys, and Modbus RTU (FC3 read / FC6 write)** as the Elite 300. Bluetti's own
materials say the app can **start/stop charging over Bluetooth** while the engine runs
(via the D+ signal wire), so a charge-enable register exists — it just hasn't been
mapped. If step 1 below confirms the handshake, everything else follows from the
existing code.

---

## Field procedure (run at the van)

### Prereqs
- Laptop with this repo + PlatformIO (`pio` on PATH). See `DEPLOY.md` for flash/monitor
  commands and the Windows UTF-8 (`charmap`) fix.
- Charger 2 powered, **engine running** (control only works while alternator/D+ active).
- Phone with the BLUETTI app, paired to the Charger 2, so we can toggle "start/stop
  charging" and watch which register changes. **Only one BLE client at a time** — keep
  the app disconnected while our tool is connected, and vice-versa.

### Step 1 — Confirm the BLE transport (BluettiRecon)
1. Get the Charger 2's BLE name/MAC: open the BLUETTI app or do a scan. Note the
   advertised name prefix and MAC.
2. Flash the recon tool and connect to the Charger 2:
   ```sh
   pio run -d Tools\BluettiRecon -e s3_recon -t upload -t monitor
   ```
   (Reflash the HMI firmware afterwards — recon is a temporary flash.)
3. **Pass/continue if:** it exposes service `0xFF00` with `0xFF01` (notify) + `0xFF02`
   (write) and the **v2 handshake completes** with the existing universal keys
   (CHALLENGE → PEER_PUBKEY → PUBKEY_ACCEPTED — see BLUETTI.md "Encryption").
4. **If the handshake fails / different service UUIDs:** the Charger 2 uses a different
   protocol or keys. Capture a phone↔charger BLE HCI snoop log (Android dev-options
   "Bluetooth HCI snoop log" or iOS PacketLogger) and stop — that's a bigger RE job,
   note it and bring the log back.

### Step 2 — Find the charge-enable register (read-correlate sweep)
Same method that found Elite regs 2011/2012/2083 (see BLUETTI.md "Discovery method").
We need a discovery harness that connects to the Charger 2's MAC, reuses the existing
crypto, and does an FC3 sweep — see "Code to build" below.

1. Baseline: wide FC3 sweep (block-read qty 10 across `0–6000`, like the old
   `wide.py`). Save raw values.
2. In the BLUETTI app, toggle **start/stop charging** (and any charge-current / mode
   sliders).
3. Re-sweep, **diff** against baseline. The charge on/off bit is most likely a single
   register in the **`2000–2089` control block** (Elite's output toggles + modes all
   live there). Charge-current / mode are likely nearby (cf. Elite 2020 charge mode,
   2214 custom current, 2083 charge limit).
4. Confirm the candidate by writing FC6 `1`/`0` to it and watching the physical
   charger + app reflect it. **Read the register back to confirm** — Bluetti commits
   writes ~0.4 s after the echo (the Elite "commit lag" gotcha; BLUETTI.md "Control
   writes").

### Step 3 — Bonus: map the DC-in regs on the Elite
While the Charger 2 is feeding the Elite 300's **DC input**, sweep the Elite (not the
charger) to finally confirm the **still-unmapped DC-in voltage/current** — the AC180
family maps these to **1213 / 1214 ×0.1** (BLUETTI.md:135, :189). This closes a known
gap regardless of how the charger control goes.

### Record results
Write findings (confirmed registers, raw diffs, anything surprising) back into this
file and into `BLUETTI.md`'s register map. Update the memory note `bluetti-charger2`.

---

## Code to build (do on the laptop, before going out or in the field)

A **MAC-parameterised discovery harness** that reuses the existing Elite crypto:

- Easiest: a new build env (e.g. `Tools/BluettiRecon` extended, or a `-e charger2`
  env) that takes a target MAC and:
  1. connects + runs the v2 handshake (reuse `src/bluetti_crypt.{h,cpp}` +
     `include/logic/modbus.h` verbatim),
  2. does the block FC3 sweep and dumps registers over serial,
  3. accepts a serial command to FC6-write `reg value` for confirming a candidate.
- Keep it **out of the main HMI firmware** until the register is known — then add a
  Charger 2 tile/toggle to the Power screen (`src/ui.cpp`) and a small client in
  `src/bluetti.cpp` (second connection target, or sequential, mindful of single-client
  BLE).

## Reference pointers
- `HMI_Screen/docs/BLUETTI.md` — protocol, crypto, Modbus, full Elite register map,
  discovery method, control-write reliability gotchas. **Read this first.**
- `HMI_Screen/src/bluetti_crypt.{h,cpp}` — handshake + AES + Modbus command build.
- `HMI_Screen/src/bluetti.{h,cpp}` — NimBLE client + poller (model for a 2nd client).
- `HMI_Screen/include/logic/modbus.h` — pure Modbus/CRC helpers (unit-tested).
- `Tools/BluettiRecon` — BLE GATT recon (temporary flash; reflash HMI after).
- `DEPLOY.md` — flash/monitor commands + Windows flashing fixes.
- Lib reference: <https://github.com/Patrick762/bluetti-bt-lib> (device defs in
  `bluetti_bt_lib/devices/`; AC180 family = closest cousin for register layout).

## Open questions to resolve in the field
- Does the Charger 2 speak the same `0xFF00`/v2/universal-key stack? (Step 1.)
- Which register is charge on/off? Single bit or value? (Step 2.)
- Does it accept control only while engine/D+ active, and does it drop BLE when the
  engine stops?
- Are charge-current / solar-vs-alternator priority also writable registers worth
  exposing on the HMI?
