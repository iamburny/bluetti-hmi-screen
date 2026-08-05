# Bluetti HMI Screen

ESP32-S3 touch dashboard for a Bluetti Elite 300 power station: connects
directly over BLE, decrypts the vendor "v2" protocol **on-device** (no bridge,
no Raspberry Pi, no cloud, no licence file), and reads/controls it over
Modbus. State of charge, solar/DC input, AC/DC load, AC/DC output on/off,
charge mode, ECO settings, and a power-flow history chart all live on the
screen. Fully offline — there's no WiFi in this build, so BLE has the radio
to itself.

This is a trimmed fork of a larger campervan control-panel project, kept down
to just the Bluetti functionality.

## Features

- **Power screen (home)** — SoC ring gauge, DC/AC in/out cards, AC/DC output
  toggles with a confirm step, charge-mode cycling, and a "release for app"
  button (the Elite 300 only accepts one BLE client at a time).
- **Power history chart** — four flows (solar/DC-in, AC-in, DC-out, AC-out)
  plus SoC, over 1h/6h/24h windows, with tappable legend filters. Backed by a
  24h PSRAM ring buffer and a best-effort SD CSV log that survives reboots.
- **Bluetti settings** — AC/DC ECO, Power Lifting, charge limit, screen
  timeout, and BLE-MAC pairing override, all read/written live over BLE.
- **On-device encrypted BLE handshake + Modbus**, all on the ESP32-S3 — see
  `docs/BLUETTI.md` for the full protocol writeup.

## Hardware

- Guition JC3248W535EN — ESP32-S3 (16MB flash, 8MB OPI PSRAM), 320×480 IPS
  LCD over QSPI (AXS15231B driver), I2C capacitive touch.
- Power source: Bluetti Elite 300 (BLE).

Full pin map is in [`CLAUDE.md`](CLAUDE.md).

## Building

Requires [PlatformIO Core](https://platformio.org/). All commands run from
the repo root; the serial monitor is 115200 baud.

```sh
# Build, flash, and monitor
pio run -e jc3248w535 -t upload -t monitor

# Run the host-side unit tests (pure logic) — needs gcc/g++ on PATH
pio test -e native
```

Unit tests also run in CI on every push/PR
([`.github/workflows/test.yml`](.github/workflows/test.yml)).

## Documentation

- [`CLAUDE.md`](CLAUDE.md) — architecture, build constraints, pin map, gotchas.
- [`docs/BLUETTI.md`](docs/BLUETTI.md) — Bluetti BLE protocol, encryption, and
  register map.
- [`docs/CHARGER2.md`](docs/CHARGER2.md) — research plan for future Bluetti
  Charger 2 support (not yet implemented).

## Credits

Bluetti BLE decryption is based on the open-source community reverse-engineering in
[`Patrick762/bluetti-bt-lib`](https://github.com/Patrick762/bluetti-bt-lib)
(originally from `nhurman/bluetti_mqtt`).
