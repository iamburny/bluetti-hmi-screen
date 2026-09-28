# Bluetti Elite 300 simulator

A PC-side stand-in for the Elite 300. It advertises the Bluetti BLE service and
speaks the encrypted "v2" protocol, so the HMI's real scan, handshake, poll, and
write path runs against it. There is no real power station in the loop.

The normal firmware checks the device's signature against Bluetti's key. Only
Bluetti has the private half of that key, so this tool signs with the app key
instead, and only the `jc3248w535_sim` firmware accepts that. The rest of the
crypto and Modbus path is the same as a real unit.

## Setup

From this directory, with Python 3.11:

```sh
python -m venv .venv
.venv\Scripts\pip install -r requirements.txt
```

## Run

```sh
.venv\Scripts\python bluetti_sim.py
```

The window shows "advertising" once the PC is on the air. Sliders are the live
telemetry (SoC, watts, AC voltage and frequency). The controls on
the right are the registers the HMI writes; a tap on the screen flips them here
once the unit's commit delay has passed (0.4 s, same as the real Elite 300).
The lifetime energy counters (grid charging, solar/DC charging, AC output) build
up from the power flows; type a kWh value and press Enter to set one. The
"ECO & limits" panel drives the values the HMI's read-only ECO & Limits page
shows (ECO timers and thresholds, SoC range, working mode, and register 2075
as a raw number).
"Simulate battery" moves SoC and time remaining from the net power and the
unit's 3024 Wh capacity. "Disconnect client" drops the link so the HMI's
reconnect path runs.

## Flash the screen

The simulator build ignores any saved Bluetti MAC and also matches the
advertised service UUID, because Windows cannot put a name in the advert.
The saved MAC is left alone, so flashing the normal env again talks to the
real unit.

```sh
pio run -e jc3248w535_sim -t upload --upload-port COM9
```

Flash back with:

```sh
pio run -e jc3248w535 -t upload --upload-port COM9
```

A sim-flashed screen shows a "SIM" tag on the power page.

## Tests

The handshake and Modbus exchange are tested in-process, with no radio:

```sh
.venv\Scripts\python -m pytest
```
