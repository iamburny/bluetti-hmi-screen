# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

# Project Context: Bluetti HMI Screen

ESP32-S3 firmware (PlatformIO/Arduino) for a touch dashboard that reads and
controls a Bluetti Elite 300 power station over BLE — natively decrypted
on-device, no bridge, no cloud, no licence file. Fully offline: there is no
WiFi in this build, so BLE has the 2.4 GHz radio entirely to itself.

This is a trimmed fork of a larger campervan control-panel project, kept down
to just the Bluetti functionality (the only part actually in daily use).

Companion doc: `docs/BLUETTI.md` — full Bluetti BLE protocol, crypto, and
register map (read this for depth — don't duplicate it here). `docs/CHARGER2.md`
is a research/field-work plan for future Bluetti Charger 2 support (not yet
implemented).

## Build / flash / test

PlatformIO Core. `pio` is at `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`
(on PATH via the PowerShell profile). All commands run from the repo root;
serial monitor @ 115200.

```sh
# Build, flash, and monitor
pio run -e jc3248w535 -t upload -t monitor

# Host-side unit tests (pure logic in include/logic/) — needs host gcc/g++ on PATH
pio test -e native
```

CI (`.github/workflows/test.yml`) runs `pio test -e native` on every push/PR
(gcc preinstalled on the runner; no local toolchain needed).

Hardware-coupled code (display, touch I2C, BLE, NVS) is **not** unit-testable
— it's verified by flashing and observing. `include/logic/` holds the pure,
hardware-free logic (touch coordinate mapping, hit-testing, Modbus framing)
shared by firmware and native tests in `test/test_logic/`.

## Repository layout

```
include/logic/   pure, hardware-free logic shared by firmware + tests
  touch_map.h    AXS15231 touch-frame parse + rotation transform
  geom.h         inRect hit-testing
  modbus.h       Modbus RTU framing/CRC + bluetti_clamp_soc
include/assets/  bluetti_logo.h (bitmap asset; not currently wired into the UI)
test/test_logic/ Unity host tests for include/logic/
docs/            BLUETTI.md (protocol reference), CHARGER2.md (future work)
src/             firmware modules (see Architecture below)
```

## Firmware architecture (`src/`)

`main.cpp` is a single-core Arduino `loop()` that polls touch and dispatches
to `ui.cpp`. Modules are flat `*.{h,cpp}` pairs owning one concern; they
communicate through small C-style functions and shared structs, not classes.

- `main.cpp` — `setup()` init order: `settings_load()` → `display_init()` →
  `ui_init()` → `touch_init()` → `bluetti_begin()` → `powerlog_init()`.
  `loop()` does rising-edge tap latching + **release debounce** (≥3 empty
  reads before "released", or single taps double-fire) and drag dispatch
  (currently a no-op — no continuous controls in this build).
- `display.{h,cpp}` — AXS15231B over QSPI wrapped in `Arduino_Canvas` (PSRAM
  framebuffer). **Rotation is applied to the canvas, not the driver** (driver
  stays 320×480 / rotation 0); `SCREEN_ROTATION` in `display.h` (1 = landscape).
- `touch.{h,cpp}` — AXS15231 cap-touch driven **directly over I2C @ 0x3B** (no
  touch library). Per-rotation coordinate transform mirrors `SCREEN_ROTATION`.
- `ui.{h,cpp}` — screen-state machine, full-height (no status bar): `POWER`
  (home screen — SoC gauge, DC/AC in/out cards, output toggles, charge-mode
  button), `POWER_CHART` (power-flow history), `BT_SETTINGS` (a top utility
  row for history + app-release, always available regardless of connection
  state, then ECO/charge-limit/screen-timeout/BLE-MAC pairing once live data
  arrives). The settings gear sits in the gap between the AC IN and AC OUT
  cards on the Power screen; a Bluetooth link icon (flashes while connecting)
  only appears on the "Bluetti offline" state, since that's the only time
  link status is otherwise invisible. `theme.h` = shared palette.
- `bluetti.{h,cpp}` + `bluetti_crypt.{h,cpp}` — native BLE client + on-device
  decryption (see Bluetti section).
- `power.{h,cpp}` — shared `PowerData` struct + `power_valid()` / `power_soc()`.
- `powerlog.{h,cpp}` — power-flow history: PSRAM ring buffer (feeds the
  on-device chart screen) + SD CSV (`/logs/power.csv`, uptime-seconds
  timestamps — there's no NTP in this build, so no dated-file rollover).
  Sampled from `loop()` off `power` on each fresh BLE poll. At boot it
  replays the CSV back into the ring so the chart survives reboots.
- `settings.{h,cpp}` — NVS-persisted config via `Preferences`: `bluettiMac`
  (BLE MAC override, blank = auto-discover by name), `chartSeriesMask` /
  `chartWindow` (power-chart trace visibility + time span).
- `keyboard.{h,cpp}` — modal on-screen QWERTY, used solely for editing the
  Bluetti BLE-MAC field in `BT_SETTINGS`.
- `sdcard.{h,cpp}` — tiny SD_MMC mount singleton for `powerlog`'s CSV.
- `include/logic/` — pure helpers, the **single source of truth** shared by
  firmware and tests: `touch_map.h` (AXS frame parse + rotation), `geom.h`
  (hit-testing), `modbus.h` (Modbus RTU framing/CRC).

## Bluetti Elite 300 — native BLE (on-device, no bridge/licence)

The firmware is a BLE **client** that connects to the Elite 300, completes the
encrypted "v2" handshake, and decrypts telemetry **on-device** using mbedTLS.
This is the open-source community crypto (universal hardcoded keys) — **not**
the closed official `.so` and **not** a bridge/Raspberry Pi. A FreeRTOS task
(core 0) holds one persistent connection, polls every 3 s (`POLL_MS` in
`bluetti.cpp`; no WiFi in this build to share the radio with, so it can poll
faster than the parent multi-feature project did), and fills
`PowerData` (SoC, DC/AC in/out watts). Commands ride the encrypted channel as
**Modbus RTU** (read FC3, write FC6); regs 2011/2012 toggle AC/DC output. Only
one BLE client at a time — `bluetti_release(seconds)` drops the link so the
phone app can connect. **Full protocol, crypto steps, and register map are in
`docs/BLUETTI.md`.**

**Control writes (reliability — see BLUETTI.md for the full story):** the Elite 300
echoes a write immediately but **commits the value ~0.4 s later**, so `writeReg()`
**confirms by reading the register back** until it reflects the new value. The UI
no longer updates optimistically — it shows a **spinner** until the next poll
confirms, then settles to the real value. The BLE connection uses a **short
~4 s supervision timeout** (`setConnectionParams`) so reconnect-after-reboot is
fast. ⚠️ Writes were once silently dropped in release builds because
`writeReg()` was called *inside* a `BDBG(...)` arg, which compiles out when
`BLUETTI_DEBUG=0` — **never put side-effecting calls inside a log macro.**

## Build constraints & gotchas

- **PSRAM:** all framebuffers/graphics must use the 8MB OPI PSRAM
  (`-DBOARD_HAS_PSRAM`, `memory_type = qio_opi`) or you get tearing / heap panics.
- **NimBLE:** use **NimBLE-Arduino 2.x** on arduino-esp32 core 3.x — 1.4.x crashes
  at init (LoadProhibited).
- **Log macros must be side-effect-free:** `BDBG(...)` compiles to
  `do{}while(0)` in release — never pass a function *call* as an argument (the call
  vanishes). Cost a long debug session in the parent project (writes silently
  no-op'd in release).
- **MAC read:** `WiFi.macAddress()`-style helpers can return all-zeros right
  after radio init — not applicable to normal operation here since there's no
  WiFi, but relevant if you ever add a MAC-printing utility.

## Pin mapping (JC3248W535EN) — verified from vendor pincfg.h

- **Backlight (LEDC PWM):** GPIO 1
- **Touch I2C:** SDA=4, SCL=8 (shared bus, AXS touch @ 0x3B); **RST: none (-1)**, INT=3
- **QSPI Display:** CS=45, CLK=47, MOSI=21, MISO=48, Data2=40, Data3=39
- **SD Card (SD_MMC, 1-bit):** CLK=12, CMD=11, D0=13
- ⚠️ **The touch I2C bus (GPIO4/8) is NOT routed to any external connector** — the
  vendor uses it internally only (confirmed by the JC3248W535 community).
